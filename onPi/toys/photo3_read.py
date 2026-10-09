#!/usr/bin/env python3
"""Print three raw 10-bit ADC values at 3 Hz: A=25/A11, B=26/A12, C=27/A13.

Run on the Pi with halo_teensy_firmware revision 8 or later.
Uses /dev/serial0 at 115200 baud, 8N1, with 3.3 V UART signals.
Pi GPIO14/TX (header 8) -> Teensy 0/RX1;
Pi GPIO15/RX (header 10) <- Teensy 1/TX1; connect a common GND.
Disable the Pi serial login console and enable the header UART.
Modules: VCC=3.3 V, shared GND, AO to the corresponding Teensy pin.
Only one Teensy serial client should run at a time.
"""

import argparse
from datetime import datetime
import re
import signal
import sys
import time

import serial


def exchange(port, command):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    reply = port.readline().decode("ascii", errors="replace").strip()
    if not reply or reply.startswith("ERROR"):
        raise RuntimeError(f"{command}: {reply or 'no reply from Teensy'}")
    return reply


def parse_adc(reply):
    match = re.fullmatch(r"ADC3 ([0-9]{1,4}) ([0-9]{1,4}) ([0-9]{1,4})", reply)
    if not match:
        raise RuntimeError(f"Invalid ADC3 reply: {reply!r}")
    values = tuple(int(value) for value in match.groups())
    if any(value > 1023 for value in values):
        raise RuntimeError(f"Invalid 10-bit ADC reply: {reply!r}")
    return values


def stop_signal(signum, frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/serial0",
                        help="Pi UART device (default: /dev/serial0)")
    parser.add_argument("--samples", type=int, default=0,
                        help="Stop after this many readings; 0 runs until Ctrl+C")
    args = parser.parse_args()
    if args.samples < 0:
        parser.error("--samples must be nonnegative")
    device = args.port
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop_signal)
    with serial.Serial(device, 115200, timeout=0.4, write_timeout=0.4,
                       exclusive=True) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        version = exchange(port, "VERSION")
        if (not version.startswith("HALO_TEENSY/1.0;")
                or "board=Teensy 4.1;" not in version
                or "UART1=115200,8N1;" not in version
                or "ADC3=25,26,27;" not in version
                or not re.search(r"(?:^|; )ADC_BITS=10(?:;|$)", version)):
            raise RuntimeError("Install ADC3 UART companion firmware revision 8 or later "
                               "with ADC3=25,26,27 and 10-bit ADC. "
                               "Received: " + version)
        # This reader never issues a drive command or refreshes the motor watchdog.
        if exchange(port, "STOPB") != "OK":
            raise RuntimeError("Motor standby was not acknowledged")
        print(version, flush=True)
        print("A=pin 25/A11, B=pin 26/A12, C=pin 27/A13; 3 Hz; raw ADC 0–1023. "
              "Ctrl+C stops.", flush=True)
        count = 0
        deadline = time.monotonic()
        while args.samples == 0 or count < args.samples:
            time.sleep(max(0.0, deadline - time.monotonic()))
            a, b, c = parse_adc(exchange(port, "ADC3"))
            stamp = datetime.now().isoformat(timespec="milliseconds")
            print(f"{stamp}  A={a:4d}  B={b:4d}  C={c:4d}", flush=True)
            count += 1
            deadline += 1.0 / 3.0
            if deadline < time.monotonic():
                deadline = time.monotonic() + 1.0 / 3.0


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Stopped.")
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print("Error: " + str(exc), file=sys.stderr)
        sys.exit(1)
