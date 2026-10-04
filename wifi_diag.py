#!/usr/bin/env python3
"""Capture Pi network failures without changing network configuration."""
import argparse
import datetime as dt
import json
import os
from pathlib import Path
import shlex
import shutil
import socket
import subprocess
import time

os.environ["PATH"] = os.environ.get("PATH", "") + ":/usr/sbin:/sbin:/usr/bin:/bin"


def nonnegative(value):
    number = int(value)
    if number < 0:
        raise argparse.ArgumentTypeError("must be zero or greater")
    return number


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seconds", type=nonnegative, default=30,
                        help="watch for changes after the initial snapshot (default: 30)")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent / "robot0"
    stamp = dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = root / "out" / "raspberry_pi" / ("wifi_diag_" + stamp + "_" + str(os.getpid()))
    out.mkdir(parents=True)
    report = out / "report.txt"
    print("Saving diagnostics to:", out, flush=True)
    print("Collecting evidence; network settings will not be changed.", flush=True)
    failures = []
    counter = 0

    def save(path, text):
        with path.open("w") as file:
            file.write(text)
            file.flush()
            os.fsync(file.fileno())

    def note(text):
        with report.open("a") as file:
            file.write(text + "\n")
            file.flush()
            os.fsync(file.fileno())

    def run(label, command, timeout=15):
        nonlocal counter
        counter += 1
        path = out / ("%03d_%s.txt" % (counter, label))
        header = dt.datetime.now().astimezone().isoformat() + "\n$ " + shlex.join(command) + "\n"
        save(path, header + "Command started; result pending.\n")
        print("  " + label, flush=True)
        status = 0
        output = ""
        try:
            if not shutil.which(command[0]):
                raise FileNotFoundError(command[0])
            result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    text=True, errors="replace", timeout=timeout)
            status, output = result.returncode, result.stdout
        except subprocess.TimeoutExpired as exc:
            status = 124
            output = exc.stdout or ""
            if isinstance(output, bytes):
                output = output.decode(errors="replace")
            output += "\nTimed out after %s seconds.\n" % timeout
        except OSError as exc:
            status, output = 127, str(exc) + "\n"
        save(path, header + output + "\nExit status: %s\n" % status)
        note("%s: exit=%s" % (path.name, status))
        if status:
            failures.append(path.name)
        return output if status == 0 else ""

    def sysfs(label):
        data = {}
        paths = [Path("/proc/net/wireless"), Path("/proc/uptime")]
        for interface in Path("/sys/class/net").iterdir():
            paths += [interface / name for name in
                      ("operstate", "carrier", "carrier_changes", "carrier_up_count", "carrier_down_count", "address")]
            for name in ("rx_errors", "tx_errors", "rx_dropped", "tx_dropped"):
                paths.append(interface / "statistics" / name)
        for device in Path("/sys/bus/usb/devices").glob("*"):
            paths += [device / name for name in
                      ("idVendor", "idProduct", "product", "power/control", "power/runtime_status")]
        for path in paths:
            try:
                if path.exists():
                    data[str(path)] = path.read_text().strip()
            except OSError as exc:
                data[str(path)] = str(exc)
        save(out / (label + ".json"), json.dumps(data, indent=2) + "\n")

    note("Host: " + socket.gethostname())
    note("Started: " + dt.datetime.now().astimezone().isoformat())
    note("Read-only diagnostics. Failed commands and unavailable previous-boot logs are retained.")
    try:
        # Capture the most perishable evidence before probing connectivity.
        run("kernel_current_boot", ["journalctl", "-b", "-k", "--no-pager", "-o", "short-iso"])
        run("network_current_boot", ["journalctl", "-b", "-u", "NetworkManager", "-u",
                                    "wpa_supplicant", "--no-pager", "-o", "short-iso"])
        run("addresses", ["ip", "-details", "-statistics", "address"])
        routes = run("routes_json", ["ip", "-j", "-4", "route", "show", "table", "all"])
        run("wifi_link", ["iw", "dev", "wlan0", "link"])
        run("power_flags", ["vcgencmd", "get_throttled"])
        sysfs("initial_sysfs")
        commands = [
            ("boot_history", ["journalctl", "--list-boots", "--no-pager"]),
            ("kernel_previous_boot", ["journalctl", "-b", "-1", "-k", "--no-pager", "-o", "short-iso"]),
            ("network_previous_boot", ["journalctl", "-b", "-1", "-u", "NetworkManager", "-u",
                                       "wpa_supplicant", "--no-pager", "-o", "short-iso"]),
            ("kernel_ring", ["dmesg", "--ctime"]),
            ("system", ["uname", "-a"]),
            ("uptime", ["uptime"]),
            ("memory", ["free", "-h"]),
            ("disk", ["df", "-h", str(out)]),
            ("temperature", ["vcgencmd", "measure_temp"]),
            ("rfkill", ["rfkill", "list"]),
            ("devices", ["nmcli", "device", "status"]),
            ("device_details", ["nmcli", "device", "show"]),
            ("connections", ["nmcli", "-f", "NAME,UUID,TYPE,DEVICE", "connection", "show"]),
            ("cached_access_points", ["nmcli", "-f", "IN-USE,SSID,BSSID,CHAN,FREQ,SIGNAL,SECURITY",
                                      "device", "wifi", "list", "--rescan", "no"]),
            ("wifi_devices", ["iw", "dev"]),
            ("wifi_stations", ["iw", "dev", "wlan0", "station", "dump"]),
            ("wifi_power_save", ["iw", "dev", "wlan0", "get", "power_save"]),
            ("wifi_country", ["iw", "reg", "get"]),
            ("ethernet_link", ["ethtool", "eth0"]),
            ("ethernet_driver", ["ethtool", "-i", "eth0"]),
            ("wifi_driver", ["ethtool", "-i", "wlan0"]),
            ("usb_devices", ["lsusb"]),
            ("usb_tree", ["lsusb", "-t"]),
            ("modules", ["lsmod"]),
            ("ipv4_routes", ["ip", "-4", "route", "show", "table", "all"]),
            ("ipv6_routes", ["ip", "-6", "route", "show", "table", "all"]),
            ("routing_rules", ["ip", "rule"]),
            ("neighbors", ["ip", "neigh"]),
        ]
        for label, command in commands:
            run(label, command)
        try:
            gateways = sorted({(r["dev"], r["gateway"]) for r in json.loads(routes or "[]")
                               if r.get("dst") == "default" and r.get("dev") and r.get("gateway")})
        except (ValueError, TypeError, KeyError):
            gateways = []
        for interface, gateway in gateways:
            run("gateway_" + interface, ["ping", "-n", "-I", interface, "-c", "3", "-W", "2", gateway], 10)
        deadline = time.monotonic() + args.seconds
        print("Watching for changes for %s seconds..." % args.seconds, flush=True)
        while time.monotonic() < deadline:
            run("watch_addresses", ["ip", "-br", "address"])
            run("watch_wifi", ["iw", "dev", "wlan0", "link"])
            run("watch_power", ["vcgencmd", "get_throttled"])
            time.sleep(max(0, min(10, deadline - time.monotonic())))
        sysfs("final_sysfs")
        run("kernel_at_end", ["journalctl", "-b", "-k", "--no-pager", "-o", "short-iso"])
        run("network_at_end", ["journalctl", "-b", "-u", "NetworkManager", "-u",
                              "wpa_supplicant", "--no-pager", "-o", "short-iso"])
        note("Completed: " + dt.datetime.now().astimezone().isoformat())
    except KeyboardInterrupt:
        note("Interrupted by user; previously saved evidence is intact.")
        print("\nInterrupted; collected evidence is saved.", flush=True)
    if failures:
        note("Commands with nonzero status: " + ", ".join(failures))
        print("Some checks failed or were unavailable; their output is saved for diagnosis.")
    print("Saved:", out)
    print("Capture finished. Collect BEFORE rebooting or restarting Wi-Fi next time.")


if __name__ == "__main__":
    main()
