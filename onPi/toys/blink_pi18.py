#!/usr/bin/env python3
"""1 Blink a grounded LED on physical header pin 18 (BCM GPIO24).

Wiring: pin 18 -> 5 kohm resistor -> LED anode; LED cathode -> Pi GND.
Requires the installed gpiod v2 Python binding and access to /dev/gpiochip0.
"""
import argparse
import signal
import sys
import threading

import gpiod
from gpiod.line import Direction, Value

GPIO = 24  # BCM numbering: physical header pin 18, NOT BCM GPIO18.
HALF_PERIOD = 0.5


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cycles", type=int, default=0,
                        help="number of on/off cycles; 0 blinks until Ctrl-C (default)")
    args = parser.parse_args()
    if args.cycles < 0:
        parser.error("--cycles must be nonnegative")

    stopped = threading.Event()
    def stop(_signum, _frame):
        stopped.set()
    for signum in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(signum, stop)

    try:
        with gpiod.request_lines(
            "/dev/gpiochip0", consumer="blink_pi18",
            config={GPIO: gpiod.LineSettings(direction=Direction.OUTPUT,
                                             output_value=Value.INACTIVE)},
        ) as request:
            print("Header pin 18 / GPIO24: 0.5 s ON, 0.5 s OFF. Ctrl-C to stop.", flush=True)
            completed = 0
            try:
                while not stopped.is_set() and (args.cycles == 0 or completed < args.cycles):
                    request.set_value(GPIO, Value.ACTIVE)
                    if stopped.wait(HALF_PERIOD):
                        break
                    request.set_value(GPIO, Value.INACTIVE)
                    if stopped.wait(HALF_PERIOD):
                        break
                    completed += 1
            finally:
                request.set_value(GPIO, Value.INACTIVE)
            print(f"Stopped; GPIO24 LOW (LED off). Completed cycles: {completed}", flush=True)
    except OSError as exc:
        print(f"Cannot control GPIO24: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
