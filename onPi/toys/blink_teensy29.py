#!/usr/bin/env python3
"""Blink the Teensy white LED via UART: 0.4 s on, then 0.4 s off.

Run on the Pi: python3 ~/plant_lineFollower/onPi/toys/blink_teensy29.py
Requires halo_teensy_firmware revision 16 or later with UART1 support.
Pi GPIO14/TX (header 8) -> Teensy 0/RX1;
Pi GPIO15/RX (header 10) <- Teensy 1/TX1; connect a common GND.
Use 3.3 V UART, 115200 baud, 8N1; disable the Pi serial login console.
Wire pin 21 -> 5 kohm series resistor -> LED anode; LED cathode -> GND.
The UART commands remain ON29 and OFF29; they drive pin 21.
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
        if ("board=Teensy 4.1;" not in version or "GPIO21" not in version
                or "LED_PIN=21;" not in version or "UART1=115200,8N1;" not in version):
            raise RuntimeError("Install revision-16 GPIO21 UART companion firmware. Received: " + repr(version))
        print(version, flush=True)
        print("Pin 21: 0.4 s ON, 0.4 s OFF. Ctrl+C to stop.", flush=True)
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
