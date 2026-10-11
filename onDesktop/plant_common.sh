# Shared paths for the desktop commands. Source this file; do not run it.
set -euo pipefail

PLANT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLANT_SRC="$PLANT_ROOT/lineFollowerDistributed"
PLANT_BIN="$PLANT_ROOT/lineFollowerDistributedBin"
PI_HOST="${PI_HOST:-10.0.0.149}"
PI_USER=pi
PI_ROOT=/home/pi/plant_lineFollower
PI_SRC=$PI_ROOT/lineFollowerDistributed
PI_BIN=$PI_ROOT/lineFollowerDistributedBin
PI_CALIB=$PI_ROOT/onPi/localControl/out/photo3_calib.latest.json
ARDUINO_CLI="${ARDUINO_CLI:-/Users/balewski/shared_volumes/2026_robot1/robot0/teensy41/tools/arduino-cli}"
ARDUINO_DATA="${ARDUINO_DATA:-/Users/balewski/shared_volumes/2026_robot1/robot0/teensy41/tools/arduino-data}"

ssh_pi() {
    ssh -4 -o ConnectTimeout=8 "${PI_USER}@${PI_HOST}" "$@"
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

sync_sources() {
    echo "Copying sources to pi@${PI_HOST}:$PI_SRC"
    ssh_pi "mkdir -p '$PI_SRC'"
    rsync -a --delete -e "ssh -4 -o ConnectTimeout=8" \
        "$PLANT_SRC/" "${PI_USER}@${PI_HOST}:$PI_SRC/"
}

build_mac_operator() {
    echo "Compiling Mac operator"
    make -C "$PLANT_SRC" ROLE=operator
}

build_pi_controller() {
    echo "Compiling Pi controller and gpio_clock"
    ssh_pi "make -C '$PI_SRC' ROLE=controller"
}
