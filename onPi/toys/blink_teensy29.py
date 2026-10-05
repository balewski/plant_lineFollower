#!/usr/bin/env python3
"""Blink Teensy pin 29 via UART: 0.4 s on, then 0.4 s off.

Run on the Pi: python3 ~/plant_lineFolower/onPi/toys/blink_teensy29.py
Requires halo_teensy_firmware revision 13 or later with UART1 support.
Pi GPIO14/TX (header 8) -> Teensy 0/RX1;
Pi GPIO15/RX (header 10) <- Teensy 1/TX1; connect a common GND.
Use 3.3 V UART, 115200 baud, 8N1; disable the Pi serial login console.
Wire pin 29 -> 5 kohm series resistor -> LED anode; LED cathode -> GND.
"""

import argparse
import signal
import sys
from time import sleep

import serial

PATTERN = (("ON29", 0.4), ("OFF29", 0.4))


def exchange(port, command):
    port.write((command + "\n").encode("ascii"))
    port.flush()
    reply = port.readline().decode("ascii", errors="replace").strip()
    if not reply:
        raise RuntimeError("No reply from Teensy. Check UART TX/RX wiring, common GND, "
                           "baud rate, UART firmware, and that the Pi serial console is disabled.")
    return reply


def stop(signum, frame):
    raise KeyboardInterrupt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/serial0",
                        help="Pi UART device (default: /dev/serial0)")
    parser.add_argument("--cycles", type=int, default=0,
                        help="Number of on/off cycles; 0 repeats until Ctrl+C")
    args = parser.parse_args()
    if args.cycles < 0:
        parser.error("--cycles must be nonnegative")
    device = args.port
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, stop)

    with serial.Serial(device, 115200, timeout=2, write_timeout=2, exclusive=True) as port:
        sleep(0.5)
        port.reset_input_buffer()
        version = exchange(port, "VERSION")
        if ("board=Teensy 4.1;" not in version or "GPIO29" not in version
                or "LED_PIN=29;" not in version or "UART1=115200,8N1;" not in version):
            raise RuntimeError("Install revision-13 GPIO29 UART companion firmware. Received: " + repr(version))
        print(version, flush=True)
        print("Pin 29: 0.4 s ON, 0.4 s OFF. Ctrl+C to stop.", flush=True)
        try:
            count = 0
            while args.cycles == 0 or count < args.cycles:
                for command, duration in PATTERN:
                    if exchange(port, command) != "OK":
                        raise RuntimeError("Teensy did not acknowledge " + command)
                    sleep(duration)
                count += 1
        finally:
            try:
                port.write(b"OFF29\n")
                port.flush()
            except (serial.SerialException, OSError):
                pass


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\nStopped; OFF29 sent if connected (firmware watchdog: 3 s).")
    except (serial.SerialException, OSError, RuntimeError) as exc:
        print("Error: " + str(exc), file=sys.stderr)
        sys.exit(1)
