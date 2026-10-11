"""Build overlay.json from a plant run. Transmit timing uses clock_calib.json."""
import csv
import json
import math
import sys
from datetime import datetime
from pathlib import Path
from zoneinfo import ZoneInfo

sys.path.insert(0, str(Path(__file__).resolve().parent))
from command_report import command_report
from run_bounds import run_bounds


def quartiles(values):
    values = sorted(values)
    def pick(p):
        return values[max(0, math.ceil(len(values) * p) - 1)]
    return {"n": len(values), "q1": pick(0.25), "median": pick(0.5), "q3": pick(0.75)}


def main():
    p = Path(sys.argv[1])
    desktop = p / "desktop"
    pi = p / "pi"
    dr = list(csv.DictReader((desktop / "events.csv").open()))
    pr = list(csv.DictReader((pi / "events.csv").open()))
    frames = list(csv.DictReader((p / "raw.mp4.frames.csv").open()))
    ds = json.loads((desktop / "summary.json").read_text())
    ps = json.loads((pi / "summary.json").read_text())
    cfg = ds["config"]
    armed, stop, stop_label = run_bounds(dr)
    anchor = next(r for r in dr if r["event"] == "wall_anchor")
    wall_start = int(anchor["b"]) + (armed - int(anchor["a"]))
    label = datetime.fromtimestamp(wall_start / 1e9, ZoneInfo("America/Los_Angeles")).strftime("%Y-%m-%d %H:%M")
    period = 1000.0 / ps["config"]["rate"]
    matched = bool(cfg.get("clock_matched"))
    clock = json.loads((desktop / "clock_calib.json").read_text()) if matched else None
    midpoint = offset = None
    if matched:
        midpoint = clock["mac_pi"]["wifi"]["midpoint_ms"]["median"]
        offset = clock["pi_teensy"]["nominal_offset_s"]

    adc = {}
    rx = {}
    computed = {}
    for r in dr:
        if r["event"] == "sensor_received":
            sid = int(r["a"])
            adc[sid] = [int(r["c"]), int(r["d"]), 0]
            rx[sid] = int(r["b"])
        elif r["event"] == "sensor_received_c":
            adc[int(r["a"])][2] = int(r["c"])
        elif r["event"] == "target_computed":
            computed[int(r["a"])] = int(r["b"])

    samples = {}
    last = None
    for r in pr:
        if r["event"] == "sample":
            last = int(r["a"])
            samples[last] = {"start_us": None}
        elif r["event"] == "device_sample" and last is not None:
            samples[last]["start_us"] = int(r["b"])
    teensy = {int(r["a"]): int(r["b"]) for r in pr if r["event"] == "teensy_target_rx"}

    pending = None
    completed = None
    cycles = []
    for r in dr:
        if r["event"] == "target_computed":
            pending = int(r["a"])
        elif r["event"] == "device_sample" and pending is not None:
            completed = int(r["b"])
        elif r["event"] == "decision":
            sid = int(r["a"])
            cmd = int(r["b"])
            if sid not in rx or sid not in computed or sid not in adc:
                continue
            done_us = completed if pending == sid else None
            transmit = None
            if matched and done_us is not None:
                pi_of_sample = done_us * 1000.0 + offset * 1e9
                transmit = (rx[sid] - (pi_of_sample + midpoint * 1e6)) / 1e6
            loop = None
            start_us = samples.get(sid, {}).get("start_us")
            if cmd in teensy and start_us is not None and teensy[cmd] >= start_us:
                loop = (teensy[cmd] - start_us) / 1000.0
            cycles.append({
                "show_ns": int(r["t_ns"]),
                "adc": adc[sid],
                "left": int(r["c"]),
                "right": int(r["d"]),
                "compute_ms": (computed[sid] - rx[sid]) / 1e6,
                "transmit_ms": transmit,
                "loop_ms": loop,
            })
    if not cycles:
        raise SystemExit("No desktop decisions in this run")
    cycles.sort(key=lambda c: c["show_ns"])
    times = [c["show_ns"] for c in cycles]
    transmits = [c["transmit_ms"] for c in cycles if c["transmit_ms"] is not None and math.isfinite(c["transmit_ms"])]
    loops = [c["loop_ms"] for c in cycles if c["loop_ms"] is not None]
    if matched:
        if not transmits:
            raise SystemExit("Clock matched but no transmit samples could be rebuilt")
        uncertainty = float(cfg["latency_uncertainty_ms"])
        note = "* Transmit uncertainty: +/- %.1f ms" % uncertainty
        transmit_stats = quartiles(transmits)
    else:
        metric = ds.get("metrics", {}).get("transmit_latency_ms", {})
        uncertainty = float(metric.get("median", 0))
        note = "* Older clock file. Transmit shown is the median; Q3 is p99."
        if metric.get("count"):
            transmit_stats = {
                "n": int(metric["count"]),
                "q1": float(metric["median"]),
                "median": float(metric["median"]),
                "q3": float(metric.get("p99", metric["median"])),
            }
        elif transmits:
            transmit_stats = quartiles(transmits)
        else:
            raise SystemExit("No transmit measurement in this run")
    stats = [{"label": "Transmit*", **transmit_stats}]
    stats.append({"label": "Desktop compute", **quartiles([c["compute_ms"] for c in cycles])})
    if loops:
        stats.append({"label": "Round trip", **quartiles(loops)})
        stats.append({"label": "Spare", **quartiles([period - x for x in loops])})
    stats.append({"label": "Period", **quartiles([period] * len(cycles))})

    motors = bool(ps["motors_enabled"])
    rows = []
    import bisect
    for frame in frames:
        t = int(float(frame["desktop_monotonic_ns"]))
        i = bisect.bisect_right(times, t) - 1
        if t < armed:
            state = "WAITING"
        elif t >= stop:
            state = stop_label
        else:
            state = "MOTORS ON" if motors else "MOTORS OFF"
        elapsed = max(0, min(t, stop) - armed) / 1e9
        if i < 0:
            live = ["Waiting for the first decision", "ADC / targets: --"]
        else:
            c = cycles[i]
            a = c["adc"]
            left, right = (0, 0) if t >= stop else (c["left"], c["right"])
            drive = "OFF" if (not motors or t >= stop) else "ON"
            shown = c["transmit_ms"] if c["transmit_ms"] is not None else transmit_stats["median"]
            trip = "--" if c["loop_ms"] is None else "%5.1f ms" % c["loop_ms"]
            live = [
                "ADC A %4d   B %4d   C %4d" % (a[0], a[1], a[2]),
                "Target L %+4d   R %+4d   | %s" % (left, right, drive),
                "Desktop compute %.3f ms" % c["compute_ms"],
                "Transmit %5.1f ms   Round trip %s" % (shown, trip),
                "t %.1f s" % elapsed,
            ]
        rows.append({"video_s": float(frame["video_s"]), "elapsed": "%.1f" % (elapsed * 1000), "state": state, "live": live})
    if not any(r["state"] != "WAITING" for r in rows):
        raise SystemExit("Recording ended before the job armed")
    report = command_report(pi, desktop)
    (p / "command_report.json").write_text(json.dumps(report, indent=2) + "\n")
    payload = {
        "rate_hz": ps["config"]["rate"],
        "run_label": label,
        "duration_s": (stop - armed) / 1e9,
        "uncertainty_ms": uncertainty,
        "note": note,
        "rows": rows,
        "stats": stats,
        "command_lines": report["lines"],
    }
    (p / "overlay.json").write_text(json.dumps(payload) + "\n")
    print(note)
    print("cycles", len(cycles), "round trips", len(loops), "duration_s", round(payload["duration_s"], 3))


if __name__ == "__main__":
    main()
