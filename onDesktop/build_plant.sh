#!/bin/bash
# Shared by makeBin and updateBin. makeBin passes --flash.
set -euo pipefail
flash=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --flash) flash=1; shift ;;
        --pi)
            [[ $# -ge 2 ]] || { echo "Missing value for --pi" >&2; exit 1; }
            export PI_HOST="$2"
            shift 2
            ;;
        --help|-h)
            echo "build_plant.sh [--flash] [--pi 10.0.0.20]"
            echo "Use onDesktop/makeBin or onDesktop/updateBin."
            exit 0
            ;;
        *)
            echo "Unknown option $1" >&2
            exit 1
            ;;
    esac
done
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/plant_common.sh"

if [[ "$flash" -eq 1 ]]; then
    [[ -x "$ARDUINO_CLI" ]] || die "arduino-cli not found at $ARDUINO_CLI"
    echo "Checking the Teensy USB port on the Pi"
    if ! ssh_pi "lsusb | grep -q '16c0:' && test -e /dev/ttyACM0 && test -e /dev/serial0"; then
        die "Teensy is not on the Pi. Control is /dev/serial0 (UART). Programming USB is /dev/ttyACM0 on the Pi, not on the Mac. Nothing was compiled."
    fi
    echo "Teensy UART /dev/serial0 and USB /dev/ttyACM0 are present on the Pi"
fi

sync_sources
build_mac_operator
build_pi_controller

if [[ "$flash" -eq 1 ]]; then
    echo "Compiling Teensy firmware on the Mac"
    mkdir -p "$PLANT_BIN/teensy-build"
    "$ARDUINO_CLI" --config-dir "$ARDUINO_DATA" compile --fqbn teensy:avr:teensy41 \
        --build-path "$PLANT_BIN/teensy-build" \
        "$PLANT_SRC/firmware/halo_teensy_firmware"
    hex="$(find "$PLANT_BIN/teensy-build" -name '*.hex' -print -quit)"
    [[ -n "$hex" ]] || die "Firmware compile did not produce a hex file"
    echo "Uploading $(basename "$hex") through the Pi USB port"
    scp -4 -o ConnectTimeout=8 "$hex" "${PI_SSH}:$PI_BIN/halo_teensy_firmware.hex"
    ssh_pi "bin='$PI_BIN'
loader=\$bin/teensy_loader_cli
libdir=\$bin/loader-libs
if [[ ! -x \$loader || ! -e \$libdir/libusb-0.1.so.4 ]]; then
    src=/home/pi/old-robot0/teensy41/tools
    mkdir -p \"\$libdir\"
    cp \"\$src/teensy_loader_cli\" \"\$loader\"
    chmod 755 \"\$loader\"
    cp -a \"\$src/loader-deps/usr/lib/arm-linux-gnueabihf/libusb-0.1.so.4\" \"\$libdir/\"
    cp -a \"\$src/loader-deps/usr/lib/arm-linux-gnueabihf/libusb-0.1.so.4.4.4\" \"\$libdir/\"
fi
LD_LIBRARY_PATH=\"\$libdir\" \"\$loader\" --mcu=TEENSY41 -s -w -v \"\$bin/halo_teensy_firmware.hex\""
    echo "Teensy firmware uploaded."
fi

echo "Binaries are in $PLANT_BIN on this Mac and $PI_BIN on the Pi."
