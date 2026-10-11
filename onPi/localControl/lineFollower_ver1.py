#!/usr/bin/env python3
"""Simple line follower with reverse steering using out/photo3_calib.latest.json.

Normalize each sensor: white=0, black=1. Left wheel uses 0.5+B-A;
right wheel uses 0.5+B-C, each limited to -1..1. nSigma controls tape detection
and steering updates. If all sensors are consistent with white, stop and wait
for tape. The overall time limit includes stopped waiting time.
"""
import argparse
import json
import math
from pathlib import Path
import signal
import sys
import time

import serial
from photo3_calib import SENSORS, acknowledged, read_adc, verify, stop_signal

ROOT = Path(__file__).resolve().parent
STATUS_PERIOD = 0.2  # Approximately 5 Hz console output.


def load_calibration(path):
    try:
        data = json.loads(Path(path).read_text())
        if (data.get("status") != "complete" or data.get("valid") is not True
                or data.get("black_is_high") is not True or data.get("adc_bits") != 10):
            raise ValueError("Need a complete, valid, black-high, 10-bit calibration.")
        endpoints = []
        for name, analog, pin in SENSORS:
            sensor = data["sensors"][name]
            if (sensor.get("valid") is not True or sensor.get("analog_pin") != analog
                    or sensor.get("gpio_pin") != pin):
                raise ValueError(f"Invalid calibration or wrong pin mapping for sensor {name}.")
            low, high = sensor["low_white"]["mean"], sensor["high_black"]["mean"]
            if (isinstance(low, bool) or isinstance(high, bool)
                    or not isinstance(low, (int, float)) or not isinstance(high, (int, float))
                    or not math.isfinite(low) or not math.isfinite(high)
                    or not 0 <= low < high <= 1023):
                raise ValueError(f"Invalid low/high means for sensor {name}.")
            for surface in ("low_white", "high_black"):
                sigma = sensor[surface]["stddev"]
                if (isinstance(sigma, bool) or not isinstance(sigma, (int, float))
                        or not math.isfinite(sigma) or sigma < 0):
                    raise ValueError(f"Invalid {surface} standard deviation for sensor {name}.")
            endpoints.append((float(low), float(high)))
        polarity = data["configuration"]["polarity"]
        if polarity not in ("ff", "fr", "rf", "rr"):
            raise ValueError("Invalid motor polarity in calibration.")
        return data, tuple(endpoints), polarity
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as exc:
        raise RuntimeError(f"Cannot use calibration {path}: {exc}") from exc


def clamp(value):
    return max(0.0, min(1.0, value))


def normalize(raw, endpoints):
    return tuple(clamp((value - low) / (high - low))
                 for value, (low, high) in zip(raw, endpoints))


class SigmaDeadband:
    """Hold each ADC channel until it exceeds a calibrated noise band."""
    def __init__(self, endpoints, sigmas, n_sigma):
        self.endpoints = endpoints
        self.sigmas = sigmas
        self.n_sigma = n_sigma
        self.accepted = None

    def update(self, raw, reset=False):
        if reset or self.accepted is None or self.n_sigma == 0:
            self.accepted = tuple(raw)
            return self.accepted
        updated = list(self.accepted)
        for i, (value, previous) in enumerate(zip(raw, self.accepted)):
            low, high = self.endpoints[i]
            fraction_previous = clamp((previous - low) / (high - low))
            fraction_new = clamp((value - low) / (high - low))
            sigma_white, sigma_black = self.sigmas[i]
            sigma_previous = ((1 - fraction_previous) * sigma_white
                              + fraction_previous * sigma_black)
            sigma_new = ((1 - fraction_new) * sigma_white
                         + fraction_new * sigma_black)
            # Independent reading errors: variances add for a difference.
            sigma_difference = math.hypot(sigma_new, sigma_previous)
            if abs(value - previous) > self.n_sigma * sigma_difference:
                updated[i] = value
        self.accepted = tuple(updated)
        return self.accepted


def tape_seen(raw, endpoints, sigmas, n_sigma):
    """Tape is a positive departure from any sensor's calibrated white noise."""
    return any(value > low + n_sigma * sigma_white
               for value, (low, _), (sigma_white, _) in zip(raw, endpoints, sigmas))


def gradient_noise(values, endpoints, sigmas):
    """Standard deviations for B-A and B-C in normalized units."""
    noise = [((1 - value) * white_std + value * black_std) / (high - low)
             for value, (low, high), (white_std, black_std)
             in zip(values, endpoints, sigmas)]
    return math.hypot(noise[1], noise[0]), math.hypot(noise[1], noise[2])


def significant_gradients(values, gradient_sigmas, n_sigma):
    """Ignore gradients consistent with zero under independent sensor noise."""
    a, b, c = values
    return tuple(delta if abs(delta) > n_sigma * sigma else 0.0
                 for delta, sigma in zip((b - a, b - c), gradient_sigmas))


def motor_targets(values, speed, seen, gradient_sigmas, n_sigma):
    """Return signed wheel PWM using the caller's nSigma tape decision."""
    if not seen:
        return 0, 0, False
    gradient_left, gradient_right = significant_gradients(values, gradient_sigmas, n_sigma)
    duty = round(255 * speed)
    left = round(duty * max(-1.0, min(1.0, 0.5 + gradient_left)))
    right = round(duty * max(-1.0, min(1.0, 0.5 + gradient_right)))
    return left, right, True


def wire_pwm(left, right, polarity):
    """Map signed physical wheel directions to the fixed wiring polarity."""
    if not (-255 <= left <= 255 and -255 <= right <= 255):
        raise ValueError("Wheel PWM must be within -255..255.")
    return left * (1 if polarity[0] == "f" else -1), right * (1 if polarity[1] == "f" else -1)


def run_follow(port, endpoints, args, sigmas, clock=time.monotonic, sleep=time.sleep):
    deadline = clock() + args.duration
    period = 1.0 / args.rate
    previous_seen = None
    previous_mode = None
    deadband = SigmaDeadband(endpoints, sigmas, args.n_sigma)
    next_status = clock()
    try:
        acknowledged(port, "STOPALL")
        while clock() < deadline:
            tick = clock()
            raw = read_adc(port)
            if clock() >= deadline:
                break  # Never send a new drive command after the time limit.
            raw_seen = tape_seen(raw, endpoints, sigmas, args.n_sigma)
            accepted = deadband.update(raw, reset=not previous_seen or not raw_seen)
            values = normalize(accepted, endpoints)
            gradient_sigmas = gradient_noise(values, endpoints, sigmas)
            left, right, seen = motor_targets(values, args.speed, raw_seen,
                                             gradient_sigmas, args.n_sigma)

            mode = "following" if seen else "stopped"
            previous_seen = seen

            if mode == "stopped":
                acknowledged(port, "STOPALL")
            else:
                pwm_left, pwm_right = wire_pwm(left, right, args.polarity)
                acknowledged(port, f"MOTORS {pwm_left} {pwm_right}")
            if mode != previous_mode:
                messages = {
                    "following": "Tape detected: following.",
                    "stopped": "Tape lost: stopped. Place robot on tape to resume before the time limit.",
                }
                print(messages[mode], flush=True)
                previous_mode = mode
            now = clock()
            if now >= next_status:
                print(f"ADC A={raw[0]:4d} B={raw[1]:4d} C={raw[2]:4d}; "
                      f"accepted A={values[0]:.2f} B={values[1]:.2f} C={values[2]:.2f}; "
                      f"speed L={left / 255:.3f} R={right / 255:.3f}; "
                      f"remaining={max(0, deadline-now):.1f} s; mode={mode}", flush=True)
                next_status += STATUS_PERIOD
                if next_status <= now:
                    next_status = now + STATUS_PERIOD
            next_tick = min(tick + period, deadline)
            sleep(max(0.0, next_tick - clock()))
    finally:
        acknowledged(port, "STOPALL")
    print(f"Time limit reached ({args.duration:g} s); both motors stopped.", flush=True)


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("-t", "--time", dest="duration", type=float, default=60,
                        help="Maximum control-loop time, including line-lost waiting (default: 60 s)")
    parser.add_argument("--speed", type=float, default=.5,
                        help="Maximum wheel PWM magnitude 0..1 (default: .5)")
    parser.add_argument("--nSigma", dest="n_sigma", type=float, default=2.5,
                        help="Sigma multiplier for tape detection, reading changes and steering gradients "
                             "(default: 2.5; 0 uses white means and disables update filtering)")
    parser.add_argument("--rate", type=float, default=20,
                        help="Target control rate, 10..100 Hz (default: 20)")
    parser.add_argument("--polarity", choices=("ff", "fr", "rf", "rr"),
                        help="Wheel wiring signs (default: polarity stored in calibration)")
    parser.add_argument("--port", default="/dev/serial0")
    parser.add_argument("--calibration", type=Path, default=ROOT/"out/photo3_calib.latest.json")
    parser.add_argument("--check", action="store_true",
                        help="Validate calibration, firmware and ADC while remaining stopped")
    args = parser.parse_args(argv)
    for name in ("duration", "speed", "rate", "n_sigma"):
        if not math.isfinite(getattr(args, name)):
            parser.error(f"{name} must be finite")
    if args.n_sigma < 0:
        parser.error("--nSigma must be nonnegative")
    if args.duration <= 0:
        parser.error("-t must be greater than zero")
    if not 0 <= args.speed <= 1:
        parser.error("--speed must be between 0 and 1")
    if not 10 <= args.rate <= 100:
        parser.error("--rate must be between 10 and 100 Hz")
    return args


def main(argv=None):
    args = parse_args(argv)
    # Reject invalid calibration before opening the hardware.
    calibration, endpoints, stored_polarity = load_calibration(args.calibration)
    args.polarity = args.polarity or stored_polarity
    print("Arguments (effective values):", flush=True)
    option_names = {"duration": "time", "n_sigma": "nSigma"}
    for name, value in vars(args).items():
        print(f"  --{option_names.get(name, name)} = {value}", flush=True)
    sigmas = tuple((calibration["sensors"][name]["low_white"]["stddev"],
                    calibration["sensors"][name]["high_black"]["stddev"])
                   for name, _, _ in SENSORS)
    print(f"Calibration: {args.calibration} (started {calibration.get('started', 'unknown')})")
    print(f"Wheel speed magnitude <= {args.speed:g}; reverse steering enabled; "
          f"polarity {args.polarity}; "
          f"nSigma {args.n_sigma:g}; "
          f"limit {args.duration:g} s.", flush=True)
    limits = [low + args.n_sigma * sigma_white
              for (low, _), (sigma_white, _) in zip(endpoints, sigmas)]
    print("Tape detection: any ADC > white mean + nSigma * white stddev; "
          + ", ".join(f"{name}>{limit:.2f}" for (name, _, _), limit in zip(SENSORS, limits)),
          flush=True)
    print("Steering uses B-A and B-C only when abs(gradient) > nSigma * "
          "quadrature uncertainty.", flush=True)
    print("Displayed speeds: positive = forward, negative = reverse.", flush=True)
    for sig in (signal.SIGTERM, signal.SIGHUP, signal.SIGTSTP):
        signal.signal(sig, stop_signal)
    with serial.Serial(args.port, 115200, timeout=.15, write_timeout=.15, exclusive=True) as port:
        try:
            time.sleep(.3)
            port.reset_input_buffer()
            version, raw = verify(port)
            print("Initial ADC A/B/C:", raw, "normalized:",
                  tuple(round(v, 2) for v in normalize(raw, endpoints)), flush=True)
            if args.check:
                print("Calibration, firmware and ADC checked; motors remain stopped.", flush=True)
                return 0
            run_follow(port, endpoints, args, sigmas)
        finally:
            try:
                acknowledged(port, "STOPALL")
            except (OSError, RuntimeError, serial.SerialException):
                print("Stop acknowledgement unavailable; firmware watchdog stops drive "
                      "after 750 ms without drive commands.", file=sys.stderr)
                raise
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("Stopped.", flush=True)
        sys.exit(130)
    except (OSError, RuntimeError, serial.SerialException) as exc:
        print("Error: " + str(exc), file=sys.stderr)
        sys.exit(1)
