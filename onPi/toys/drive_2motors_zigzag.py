#!/usr/bin/env python3
"""Alternate forward runs and in-place left/right turns through Pi UART.

One repetition: forward, pause, left turn, pause, forward, pause, right turn,
pause. Motor A is left; motor B is right. Positive duty follows the same
forward convention as drive_2motors_dance.py; physical direction depends on
motor wiring. --polarity selects f (unchanged) or r (reversed) for the
left and right wheels, respectively; default rr. Pauses put the driver in
standby (coast, not active braking).
Requires shared Teensy firmware with MOTOR_AB and UART1 support.
Run only one Teensy serial client at a time.
"""
import argparse
import math
import signal
import sys
import time

import serial

TICK = 0.05  # 20 Hz drive-command refresh; motor watchdog is 750 ms.


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


def run_motion(port, left, right, duration, clock=time.monotonic, sleep=time.sleep):
    started = clock()
    deadline = started + duration
    next_update = started
    while clock() < deadline:
        acknowledged(port, f"MOTORS {left} {right}")
        next_update += TICK
        now = clock()
        if next_update < now:
            next_update = now + TICK  # skip missed ticks rather than burst
        sleep(max(0.0, min(next_update, deadline) - now))


def run_sequence(port, time_forward, time_rotate, time_pause, repetitions,
                 speed=0.5, clock=time.monotonic, sleep=time.sleep, polarity="rr"):
    if polarity not in ("ff", "fr", "rf", "rr"):
        raise ValueError("polarity must be ff, fr, rf, or rr (left then right)")
    left_sign, right_sign = (1 if wheel == "f" else -1 for wheel in polarity)
    duty = round(255 * speed)
    movements = (
        ("Forward", duty, duty, time_forward),
        ("Turn left in place", -duty, duty, time_rotate),
        ("Forward", duty, duty, time_forward),
        ("Turn right in place", duty, -duty, time_rotate),
    )
    try:
        acknowledged(port, "STOPALL")
        for repetition in range(1, repetitions + 1):
            print(f"Repetition {repetition}/{repetitions}", flush=True)
            for index, (label, left, right, duration) in enumerate(movements, 1):
                print(f"  {2 * index - 1}/8: {label} ({duration:g} s)", flush=True)
                run_motion(port, left * left_sign, right * right_sign, duration,
                           clock=clock, sleep=sleep)
                acknowledged(port, "STOPALL")
                print(f"  {2 * index}/8: Pause ({time_pause:g} s)", flush=True)
                sleep(time_pause)
    finally:
        try:
            acknowledged(port, "STOPALL")
            print("Both motors stopped; driver in standby.", flush=True)
        except (serial.SerialException, OSError, RuntimeError):
            print("Stop acknowledgement unavailable; firmware stops both motors "
                  "after 750 ms without a drive command.", file=sys.stderr)


def stop_signal(signum, frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("-tf", "--timeForward", dest="time_forward", type=float,
                        default=3.0, metavar="SECONDS",
                        help="Duration of each forward run (default: 3 seconds)")
    parser.add_argument("-tr", "--timeRotate", dest="time_rotate", type=float,
                        default=1.07, metavar="SECONDS",
                        help="Duration of each in-place turn (default: 1.07 seconds)")
    parser.add_argument("-ts", "--timePause", dest="time_pause", type=float,
                        default=0.2, metavar="SECONDS",
                        help="Pause after each movement (default: 0.2 seconds; 0 allowed)")
    parser.add_argument("-r", type=int, default=10, metavar="COUNT",
                        help="Full left/right pattern repetitions (default: 10)")
    parser.add_argument("--speed", type=float, default=0.5, metavar="SCALE",
                        help="PWM scale for forward and turns, 0 to 1 (default: 0.5)")
    parser.add_argument("--polarity", choices=("ff", "fr", "rf", "rr"), default="rr",
                        help="Left/right wheel polarity: f keeps forward, r reverses it (default: rr)")
    parser.add_argument("--port", default="/dev/serial0",
                        help="Pi UART device (default: /dev/serial0)")
    parser.add_argument("--check", action="store_true",
                        help="Verify firmware and standby without driving")
    args = parser.parse_args()
    for option, value in (("--timeForward", args.time_forward),
                          ("--timeRotate", args.time_rotate)):
        if not math.isfinite(value) or value <= 0:
            parser.error(f"{option} must be finite and greater than zero")
    if not math.isfinite(args.time_pause) or args.time_pause < 0:
        parser.error("--timePause must be finite and nonnegative")
    if args.r < 1:
        parser.error("-r must be at least 1")
    if not math.isfinite(args.speed) or not 0 <= args.speed <= 1:
        parser.error("--speed must be finite and between 0 and 1")
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop_signal)
    with serial.Serial(args.port, 115200, timeout=0.25, write_timeout=0.25,
                       exclusive=True) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        version = exchange(port, "VERSION")
        if ("board=Teensy 4.1;" not in version or "MOTOR_AB" not in version
                or "UART1=115200,8N1;" not in version):
            raise RuntimeError("UART dual-motor companion firmware is required. " + version)
        print(version, flush=True)
        if args.check:
            acknowledged(port, "STOPALL")
            status = exchange(port, "STATUSAB")
            if status != "MOTORS left=0; right=0; standby=1":
                raise RuntimeError("Unexpected stopped status: " + status)
            print("Connection checked; both motors stopped and driver in standby.", flush=True)
            return
        total = args.r * (2 * args.time_forward + 2 * args.time_rotate + 4 * args.time_pause)
        print(f"{args.r} repetitions; forward {args.time_forward:g} s, "
              f"turn {args.time_rotate:g} s, pause {args.time_pause:g} s; "
              f"PWM limit {args.speed:.0%}; polarity {args.polarity}; "
              f"about {total:g} s total. Ctrl+C stops.",
              flush=True)
        run_sequence(port, args.time_forward, args.time_rotate, args.time_pause,
                     args.r, args.speed, polarity=args.polarity)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("Stopped.")
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print("Error: " + str(exc), file=sys.stderr)
        sys.exit(1)
