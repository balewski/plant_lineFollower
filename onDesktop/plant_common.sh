# Shared paths for the desktop commands. Source this file; do not run it.
set -euo pipefail

PLANT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PLANT_SRC="$PLANT_ROOT/lineFollowerDistributed"
PLANT_BIN="$PLANT_ROOT/lineFollowerDistributedBin"
PI_HOST="${PI_HOST:-10.0.0.20}"
PI_USER=pi
case "$PI_HOST" in
    10.0.0.20) PI_SSH=pi2 ;;
    10.0.0.120) PI_SSH=pi2-asus-eth ;;
    192.168.68.220) PI_SSH=pi2-home-eth ;;
    *) PI_SSH="${PI_USER}@${PI_HOST}" ;;
esac
PI_ROOT=/home/pi/plant_lineFollower
PI_SRC=$PI_ROOT/lineFollowerDistributed
PI_BIN=$PI_ROOT/lineFollowerDistributedBin
PI_CALIB=$PI_ROOT/config/photo3_calib.json
ARDUINO_CLI="${ARDUINO_CLI:-/Users/balewski/shared_volumes/2026_robot1/robot0/teensy41/tools/arduino-cli}"
ARDUINO_DATA="${ARDUINO_DATA:-/Users/balewski/shared_volumes/2026_robot1/robot0/teensy41/tools/arduino-data}"

ssh_pi() {
    ssh -4 -o ConnectTimeout=8 "$PI_SSH" "$@"
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

sync_sources() {
    echo "Copying sources to ${PI_SSH}:$PI_SRC"
    ssh_pi "mkdir -p '$PI_SRC'"
    rsync -a --delete -e "ssh -4 -o ConnectTimeout=8" \
        "$PLANT_SRC/" "${PI_SSH}:$PI_SRC/"
}

build_mac_operator() {
    echo "Compiling Mac operator and clock_probe"
    make -C "$PLANT_SRC" ROLE=operator
}

build_pi_controller() {
    echo "Compiling Pi controller, gpio_clock and clock_probe"
    ssh_pi "make -C '$PI_SRC' ROLE=controller"
}
