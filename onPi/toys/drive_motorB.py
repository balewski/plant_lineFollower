#!/usr/bin/env python3
"""Drive motor B forward through Pi UART, Teensy 4.1, and TB6612FNG.

Run on the Pi: python3 ~/plant_lineFolower/onPi/toys/drive_motorB.py
BIN1=7, BIN2=8, PWMB=9, STBY=6. Logic=3.3 V, VM=external 6 V.
Forward means BIN1 high / BIN2 low; physical direction depends on motor wiring.
Requires halo_teensy_firmware revision 7 or later with UART1 support.
Pi GPIO14/TX (header 8) -> Teensy 0/RX1;
Pi GPIO15/RX (header 10) <- Teensy 1/TX1; connect a common GND.
Use 3.3 V UART, 115200 baud, 8N1; disable the Pi serial login console.
"""

import argparse
import math
import signal
import sys
import time

import serial


def exchange(port, command):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    reply = port.readline().decode("ascii", errors="replace").strip()
    if not reply or reply.startswith("ERROR"):
        raise RuntimeError(f"Teensy command {command!r} failed: {reply or 'no reply'}")
    return reply


def acknowledged(port, command):
    reply = exchange(port, command)
    if reply != "OK":
        raise RuntimeError(f"Unexpected reply to {command!r}: {reply!r}")


def stop_signal(signum, frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--speed", type=float, default=50,
                        help="PWM duty percentage, 0–100 (default: 50)")
    parser.add_argument("--seconds", type=float, default=0,
                        help="Run time; 0 continues until Ctrl+C (default)")
    parser.add_argument("--port", default="/dev/serial0",
                        help="Pi UART device (default: /dev/serial0)")
    parser.add_argument("--check", action="store_true",
                        help="Check firmware and stopped status without driving")
    args = parser.parse_args()
    if not math.isfinite(args.speed) or not 0 <= args.speed <= 100:
        parser.error("--speed must be between 0 and 100")
    if not math.isfinite(args.seconds) or args.seconds < 0:
        parser.error("--seconds must be finite and nonnegative")
    device = args.port
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop_signal)
    duty = round(args.speed * 255 / 100)
    with serial.Serial(device, 115200, timeout=0.4, write_timeout=0.4,
                       exclusive=True) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        version = exchange(port, "VERSION")
        if "board=Teensy 4.1;" not in version or "MOTOR_B" not in version:
            raise RuntimeError("Motor-B companion firmware is required. Received: " + version)
        print(version, flush=True)
        try:
            acknowledged(port, "STOPB")
            if args.check or duty == 0:
                status = exchange(port, "STATUSB")
                if status != "MOTORB duty=0; standby=1":
                    raise RuntimeError("Unexpected stopped status: " + status)
                print("Connection checked; motor driver is in standby.", flush=True)
                return
            print(f"Motor B forward at {args.speed:g}% PWM. Ctrl+C stops. Motor A is undriven.",
                  flush=True)
            started = time.monotonic()
            while True:
                elapsed = time.monotonic() - started
                if args.seconds and elapsed >= args.seconds:
                    break
                acknowledged(port, f"MOTORB {duty}")
                delay = 0.1
                if args.seconds:
                    delay = min(delay, max(0, args.seconds - (time.monotonic() - started)))
                time.sleep(delay)
        finally:
            try:
                acknowledged(port, "STOPB")
                print("Motor B stopped; driver in standby.", flush=True)
            except (serial.SerialException, OSError, RuntimeError):
                print("Stop acknowledgement unavailable; firmware stops after 750 ms without drive commands.",
                      file=sys.stderr)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Stopped.")
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print("Error: " + str(exc), file=sys.stderr)
        sys.exit(1)
