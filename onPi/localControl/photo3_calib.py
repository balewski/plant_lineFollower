#!/usr/bin/env python3
"""Calibrate A/B/C photo sensors with slow alternating ~120-degree turns.

Place the robot symmetrically over a straight black tape on white floor.
Black is assumed HIGH ADC; white is LOW. Run only one Teensy client at a time.
Defaults: 6 sweeps, speed 0.15, polarity rr, 40 samples/s.
Angles are timing estimates based on 180 degrees in 1.07 s at speed 0.5.
Use --sweep-time to set a measured 120-degree duration at your chosen speed.
"""
import argparse
import datetime
import json
import math
import os
from pathlib import Path
import re
import signal
import statistics
import sys
import time

import serial

SENSORS = (("A", "A11", 25), ("B", "A12", 26), ("C", "A13", 27))


def exchange(port, command):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    reply = port.readline().decode("ascii", errors="replace").strip()
    if not reply or reply.startswith("ERROR"):
        raise RuntimeError(f"{command}: {reply or 'no reply from Teensy'}")
    return reply


def acknowledged(port, command):
    reply = exchange(port, command)
    if reply != "OK":
        raise RuntimeError(f"Unexpected reply to {command}: {reply!r}")


def read_adc(port):
    reply = exchange(port, "ADC3")
    match = re.fullmatch(r"ADC3 ([0-9]{1,4}) ([0-9]{1,4}) ([0-9]{1,4})", reply)
    if not match:
        raise RuntimeError(f"Invalid ADC3 reply: {reply!r}")
    values = tuple(int(value) for value in match.groups())
    if any(value > 1023 for value in values):
        raise RuntimeError(f"ADC3 values are outside 0..1023: {reply!r}")
    return values


def verify(port):
    acknowledged(port, "STOPALL")
    version = exchange(port, "VERSION")
    fields = dict(part.strip().split("=", 1) for part in version.split(";")
                  if "=" in part)
    features = fields.get("features", "").split(",")
    if (not version.startswith("HALO_TEENSY/1.0;")
            or fields.get("board") != "Teensy 4.1"
            or not {"MOTOR_AB", "ADC3"}.issubset(features)
            or fields.get("UART1") != "115200,8N1"
            or fields.get("ADC3") != "25,26,27"
            or fields.get("ADC_BITS") != "10"):
        raise RuntimeError("Need Teensy dual-motor/ADC3 UART firmware, pins 25/26/27, 10-bit ADC.")
    if exchange(port, "STATUSAB") != "MOTORS left=0; right=0; standby=1":
        raise RuntimeError("Firmware did not confirm both motors stopped.")
    return version, read_adc(port)


def turn_pwm(direction, speed, polarity):
    duty = round(255 * speed)
    left, right = (-duty, duty) if direction == "left" else (duty, -duty)
    return left * (1 if polarity[0] == "f" else -1), right * (1 if polarity[1] == "f" else -1)


def run_sweeps(port, args, record, checkpoint=lambda: None,
               clock=time.monotonic, sleep=time.sleep):
    """Motor commands and ADC requests share this one sequential UART session."""
    started = clock()
    period = 1.0 / args.rate

    def leg(phase, sweep, direction, duration):
        left, right = turn_pwm(direction, args.speed, args.polarity)
        print(f"{phase}: {direction}, {duration:.3f} s", flush=True)
        deadline = clock() + duration
        while clock() < deadline:
            tick = clock()
            acknowledged(port, f"MOTORS {left} {right}")
            values = read_adc(port)
            row = dict(zip(("A", "B", "C"), values))
            row.update(elapsed_s=round(clock() - started, 6), phase=phase,
                       sweep=sweep, direction=direction, left_pwm=left, right_pwm=right)
            record(row)
            sleep(max(0.0, min(tick + period, deadline) - clock()))
        acknowledged(port, "STOPALL")
        checkpoint()
        sleep(args.pause)

    try:
        acknowledged(port, "STOPALL")
        leg("position_left", 0, "left", args.sweep_time / 2)
        for sweep in range(1, args.sweeps + 1):
            direction = "right" if sweep % 2 else "left"
            leg("sweep", sweep, direction, args.sweep_time)
        # Odd counts end on the right; even counts end on the left.
        direction = "left" if args.sweeps % 2 else "right"
        leg("return_center", 0, direction, args.sweep_time / 2)
    finally:
        # Do not attempt an automatic return to center after an error or Ctrl+C.
        acknowledged(port, "STOPALL")


def quantile(sorted_values, fraction):
    position = (len(sorted_values) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(sorted_values) - 1)
    return sorted_values[low] + (sorted_values[high] - sorted_values[low]) * (position - low)


def plateau_stats(rows, sensor):
    values = [row[sensor] for row in rows]
    return {
        "mean": statistics.mean(values) if values else None,
        "stddev": statistics.stdev(values) if len(values) > 1 else None,
        "n": len(values),
        "sweeps": sorted({row["sweep"] for row in rows}),
    }


def analyze(rows, sweeps, min_contrast=80):
    """Select low/high plateaus; transitional samples are not calibration endpoints."""
    samples = [row for row in rows if row["phase"] == "sweep"]
    results = {}
    for sensor, analog, pin in SENSORS:
        values = sorted(row[sensor] for row in samples)
        result = {"analog_pin": analog, "gpio_pin": pin, "valid": False,
                  "low_white": plateau_stats([], sensor), "high_black": plateau_stats([], sensor),
                  "issues": []}
        results[sensor] = result
        if not values:
            result["issues"].append("No complete-sweep samples.")
            continue
        # Robust anchors ignore isolated outliers. Endpoint bands cover the
        # lowest/highest 20% of this amplitude span, NOT 20% of sample counts.
        anchor_low, anchor_high = quantile(values, .02), quantile(values, .98)
        span = anchor_high - anchor_low
        low_limit = anchor_low + .20 * span
        high_limit = anchor_high - .20 * span
        low_rows = [row for row in samples if row[sensor] <= low_limit]
        high_rows = [row for row in samples if row[sensor] >= high_limit]
        result.update(anchor_low=anchor_low, anchor_high=anchor_high,
                      low_max=low_limit, high_min=high_limit,
                      observed_min=values[0], observed_max=values[-1],
                      low_white=plateau_stats(low_rows, sensor),
                      high_black=plateau_stats(high_rows, sensor))
        issues = result["issues"]
        if span < min_contrast:
            issues.append(f"ADC span {span:.1f} < required {min_contrast:g}; no reliable contrast.")
        for label, selected in (("white/low", low_rows), ("black/high", high_rows)):
            if len(selected) < 8:
                issues.append(f"Only {len(selected)} {label} samples; need at least 8.")
            # At least two samples on a plateau in at least half the passes.
            hits = sum(sum(row["sweep"] == index for row in selected) >= 2
                       for index in range(1, sweeps + 1))
            if hits < max(2, math.ceil(sweeps / 2)):
                issues.append(f"{label} plateau seen repeatedly in only {hits}/{sweeps} sweeps.")
        transition_count = sum(low_limit < value < high_limit for value in values)
        result["transition_samples"] = transition_count
        if transition_count > .5 * len(values):
            issues.append("More than half the readings are transitions; plateaus are poorly defined.")
        low, high = result["low_white"], result["high_black"]
        if low["mean"] is not None and high["mean"] is not None:
            result["midpoint_threshold"] = (low["mean"] + high["mean"]) / 2
        result["valid"] = not issues
    return results


def format_summary(results):
    lines = ["ADC calibration: white = LOW, black tape = HIGH; raw 10-bit ADC.",
             "Std dev is sample standard deviation across selected plateau readings.",
             "Sensor/pin      LOW mean +/- std (n)       HIGH mean +/- std (n)     Quality"]
    def cell(stats):
        if stats["mean"] is None:
            return "unavailable"
        std = f"{stats['stddev']:.2f}" if stats["stddev"] is not None else "n/a"
        return f"{stats['mean']:.2f} +/- {std} ({stats['n']})"
    for sensor, result in results.items():
        lines.append(f"{sensor}/{result['analog_pin']:5s}  {cell(result['low_white']):>25s}  "
                     f"{cell(result['high_black']):>25s}  {'OK' if result['valid'] else 'RETRY'}")
        lines.extend("  " + issue for issue in result["issues"])
    return "\n".join(lines) + "\n"


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--port", default="/dev/serial0")
    parser.add_argument("--polarity", choices=("ff", "fr", "rf", "rr"), default="rr")
    parser.add_argument("--speed", type=float, default=.15, help="PWM scale, 0 < speed <= 1 (default: .15)")
    parser.add_argument("--sweeps", type=int, default=6, help="One-way 120-degree passes (default: 6)")
    parser.add_argument("--turn180", type=float, default=1.07,
                        help="Measured 180-degree time at speed 0.5 (default: 1.07 s)")
    parser.add_argument("--sweep-time", type=float,
                        help="Override seconds for a 120-degree pass at the chosen speed")
    parser.add_argument("--rate", type=float, default=40, help="Target sample/drive refresh rate, 10..100 Hz")
    parser.add_argument("--pause", type=float, default=.3, help="Stopped pause between turns (default: .3 s)")
    parser.add_argument("--countdown", type=float, default=3, help="Stopped delay before movement (default: 3 s)")
    parser.add_argument("--min-contrast", type=float, default=80, help="Minimum reliable ADC span (default: 80)")
    parser.add_argument("--check", action="store_true", help="Check firmware, standby and ADC; never drive")
    args = parser.parse_args(argv)
    for name in ("speed", "turn180", "rate", "pause", "countdown", "min_contrast", "sweep_time"):
        value = getattr(args, name)
        if value is not None and not math.isfinite(value):
            parser.error(f"--{name.replace('_', '-')} must be finite")
    if not 0 < args.speed <= 1 or round(255 * args.speed) == 0:
        parser.error("--speed must produce nonzero PWM and be at most 1")
    if args.sweeps < 2:
        parser.error("--sweeps must be at least 2")
    if args.turn180 <= 0 or (args.sweep_time is not None and args.sweep_time <= 0):
        parser.error("Turn durations must be greater than zero")
    if not 10 <= args.rate <= 100:
        parser.error("--rate must be between 10 and 100 Hz")
    if args.pause < 0 or args.countdown < 0:
        parser.error("--pause and --countdown must be nonnegative")
    if not 0 < args.min_contrast <= 1023:
        parser.error("--min-contrast must be greater than 0 and at most 1023")
    args.timing_estimated = args.sweep_time is None
    if args.sweep_time is None:
        args.sweep_time = args.turn180 * (120 / 180) * (.5 / args.speed)
    return args


def stop_signal(signum, frame):
    raise KeyboardInterrupt


def write_text(path, text):
    temporary = path.with_name("." + path.name + ".tmp")
    try:
        with temporary.open("w") as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())
        os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def save_report(out, report):
    """Replace the two latest files; raw readings are part of the JSON."""
    report["sample_count"] = len(report["samples"])
    if "sensors" in report and not report["configuration"]["check"]:
        summary = format_summary(report["sensors"])
    elif report["configuration"]["check"]:
        summary = "Check-only run; no calibration performed.\n"
    else:
        summary = "Calibration in progress; results are not yet valid.\n"
    summary = (f"Calibration started: {report['started']}\n"
               f"Status: {report['status']}; valid: {report['valid']}\n\n" + summary)
    if report.get("error"):
        summary += "Run incomplete: " + report["error"] + "\n"
    if report["status"] == "complete" and not report["valid"]:
        summary += "Calibration needs another run: check that every sensor crosses both surfaces.\n"
    write_text(out / "photo3_calib.latest.json",
               json.dumps(report, indent=2, allow_nan=False) + "\n")
    write_text(out / "photo3_calib.latest.txt", summary)
    return summary


def main(argv=None):
    args = parse_args(argv)
    started = datetime.datetime.now().astimezone().isoformat(timespec="seconds")
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGTSTP):
        signal.signal(sig, stop_signal)
    out = Path(__file__).resolve().parent / "out"
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    report = {"started": started,
              "configuration": vars(args), "status": "starting", "valid": False,
              "black_is_high": True, "adc_bits": 10, "samples": rows, "sample_count": 0,
              "method": "2nd/98th percentile anchors; bottom/top 20% amplitude bands; sample stddev",
              "notes": "Angles are timed estimates; initial/final positioning is excluded from statistics."}
    print("Results:", out / "photo3_calib.latest.json", "and photo3_calib.latest.txt", flush=True)
    print(f"Polarity {args.polarity}; speed {args.speed:g}; {args.sweeps} passes; "
          f"{args.sweep_time:.3f} s per ~120-degree sweep; target {args.rate:g} Hz.", flush=True)
    if args.timing_estimated:
        print("Timing assumes turn rate scales with PWM. If turns stall or miss the arc, "
              "adjust --speed and/or --sweep-time.", flush=True)
    # Acquire the UART before replacing previous results. A second client that
    # cannot acquire the port must not overwrite a calibration already running.
    try:
        port = serial.Serial(args.port, 115200, timeout=.15, write_timeout=.15, exclusive=True)
    except (OSError, serial.SerialException) as exc:
        print("Error: " + str(exc) + "; previous latest files were kept.", file=sys.stderr)
        return 1
    try:
        try:
            save_report(out, report)
            time.sleep(.3)
            port.reset_input_buffer()
            version, initial = verify(port)
            report.update(firmware=version, initial_adc=dict(zip(("A", "B", "C"), initial)))
            print("Initial ADC:", report["initial_adc"], flush=True)
            if args.check:
                report["status"] = "check_only"
                print("Connection and ADC checked; both motors remain stopped.", flush=True)
            else:
                print(f"Starting in {args.countdown:g} s. Ctrl+C stops.", flush=True)
                time.sleep(args.countdown)
                report["status"] = "running"
                # Checkpoints are written after STOPALL between legs.
                run_sweeps(port, args, rows.append, lambda: save_report(out, report))
                report["status"] = "complete"
        except KeyboardInterrupt:
            report.update(status="interrupted", error="Interrupted; partial readings saved.")
        except (OSError, RuntimeError, serial.SerialException) as exc:
            report.update(status="error", error=str(exc))
        finally:
            try:
                acknowledged(port, "STOPALL")
            except (OSError, RuntimeError, serial.SerialException) as exc:
                report["stop_error"] = str(exc)
                print("STOPALL was not acknowledged; firmware watchdog stops drive "
                      "after 750 ms without drive commands.", file=sys.stderr)
                if report["status"] in ("complete", "check_only"):
                    report.update(status="error", error="Final motor standby was not acknowledged")
        results = analyze(rows, args.sweeps, args.min_contrast)
        report.update(sensors=results,
                      valid=report["status"] == "complete" and all(r["valid"] for r in results.values()))
        summary = save_report(out, report)
        print(summary, end="")
        print("Saved photo3_calib.latest.json and photo3_calib.latest.txt in", out)
    finally:
        port.close()
    if report["status"] == "check_only":
        return 0
    return 0 if report["valid"] else (130 if report["status"] == "interrupted" else 1)


if __name__ == "__main__":
    sys.exit(main())
