#!/bin/bash
# Console recovery for this Pi's USB Wi-Fi. No reboot or persistent config changes.
set -u
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
BASE_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
IFACE=wlan0
PROFILE=robot-wifi
USB_DRIVER=""
USB_INTERFACE=""
REBIND_PENDING=0
RADIO_OFF=0


RECOVERY_LOGDIR=""

# Flush captured output to disk before displaying it on the console.
durable_tee() {
    python3 -u -c '
import os, sys
with open(sys.argv[1], "a", buffering=1) as log:
    for line in sys.stdin:
        log.write(line)
        log.flush()
        os.fsync(log.fileno())
        try:
            sys.stdout.write(line)
            sys.stdout.flush()
        except BrokenPipeError:
            pass
' "$1"
}

checkpoint() {
    [[ -n "$RECOVERY_LOGDIR" ]] || return 0
    python3 - "$RECOVERY_LOGDIR/events.jsonl" "$1" <<'PYLOG'
import datetime, json, os, sys
with open(sys.argv[1], "a") as f:
    f.write(json.dumps({"time": datetime.datetime.now().astimezone().isoformat(),
                        "event": sys.argv[2]}) + "\n")
    f.flush()
    os.fsync(f.fileno())
PYLOG
}

snapshot_after() {
    [[ -n "$RECOVERY_LOGDIR" ]] || return 0
    timeout -k 2 10 journalctl -b -k -n 100 --no-pager > "$RECOVERY_LOGDIR/kernel-$1.txt" 2>&1 || true
    timeout -k 2 10 journalctl -b -u NetworkManager -u wpa_supplicant -n 100 --no-pager > "$RECOVERY_LOGDIR/network-$1.txt" 2>&1 || true
    python3 - "$RECOVERY_LOGDIR" "$1" <<'PYSYNC'
import os, pathlib, sys
for kind in ("kernel", "network"):
    with open(pathlib.Path(sys.argv[1]) / (kind + "-" + sys.argv[2] + ".txt"), "rb") as f:
        os.fsync(f.fileno())
PYSYNC
}

usage() {
    cat <<'HELP'
Usage: ~/wifi_fix.sh [--check|--help]
Run from the Pi console after Wi-Fi fails. Requests sudo if needed.
Saves diagnostics, then tries reconnect, radio restart, and USB driver rebind.
Stops after Wi-Fi has an IPv4 address and can ping its gateway.
--check  Check prerequisites and current connection without changing anything.
Logs: ~/robot0/out/raspberry_pi/wifi_fix_*/
HELP
}

identify_usb() {
    local device
    device="$(readlink -f "/sys/class/net/$IFACE/device" 2>/dev/null)" || return 1
    USB_DRIVER="$(readlink -f "$device/driver" 2>/dev/null)" || return 1
    USB_INTERFACE="${device##*/}"
    [[ "$USB_DRIVER" == /sys/bus/usb/drivers/* &&
       "$USB_INTERFACE" =~ ^[0-9]+-[0-9.]+:[0-9]+\.[0-9]+$ &&
       -e "$USB_DRIVER/unbind" && -e "$USB_DRIVER/bind" ]]
}

healthy() {
    local gateway address state
    if ! state="$(nmcli -g GENERAL.STATE device show "$IFACE" 2>&1)"; then
        printf '%s\n' "$state"
        return 1
    fi
    [[ "$state" == 100* ]] || return 1
    address="$(ip -4 -o address show dev "$IFACE" scope global 2>/dev/null)"
    [[ -n "$address" ]] || return 1
    gateway="$(ip -4 route show default dev "$IFACE" | awk '/via/ {print $3; exit}')"
    [[ -n "$gateway" ]] || return 1
    if ping -n -I "$IFACE" -c 2 -W 2 "$gateway"; then
        echo "Wi-Fi connection verified: $address"
        echo "Gateway $gateway responds through $IFACE."
        return 0
    fi
    return 1
}

connect_wifi() {
    timeout 40 nmcli --wait 30 connection up id "$PROFILE" ifname "$IFACE"
}

cleanup() {
    if (( RADIO_OFF )); then
        timeout 10 nmcli radio wifi on || true
        RADIO_OFF=0
    fi
    # If interrupted between unbind and bind, put the Wi-Fi driver back.
    if (( REBIND_PENDING )); then
        echo "Restoring USB Wi-Fi driver..."
        if printf '%s' "$USB_INTERFACE" > "$USB_DRIVER/bind"; then
            REBIND_PENDING=0
        else
            echo "Driver rebind failed; unplug and reinsert the Wi-Fi dongle."
        fi
    fi
}

reset_usb() {
    if ! identify_usb; then
        echo "Cannot safely identify a USB Wi-Fi interface; skipping USB reset."
        return 1
    fi
    echo "Rebinding $USB_INTERFACE to ${USB_DRIVER##*/}."
    checkpoint "usb-unbind-start"
    REBIND_PENDING=1
    if ! printf '%s' "$USB_INTERFACE" > "$USB_DRIVER/unbind"; then
        REBIND_PENDING=0
        return 1
    fi
    checkpoint "usb-unbound"
    sleep 2
    checkpoint "usb-bind-start"
    if ! printf '%s' "$USB_INTERFACE" > "$USB_DRIVER/bind"; then
        cleanup
        return 1
    fi
    REBIND_PENDING=0
    checkpoint "usb-bound"
    # Wait for NetworkManager to discover the recreated interface.
    local attempt
    for attempt in {1..10}; do
        if nmcli -g GENERAL.STATE device show "$IFACE" >/dev/null 2>&1; then
            sleep 2
            return 0
        fi
        sleep 1
    done
    return 1
}

recover() {
    checkpoint "step-1-reconnect-start"
    echo "1/3: Reconnecting saved Wi-Fi profile..."
    timeout 15 nmcli --wait 10 device disconnect "$IFACE" || true
    timeout 10 nmcli radio wifi on || true
    connect_wifi || true
    snapshot_after reconnect
    if healthy; then checkpoint "recovered"; return 0; fi

    checkpoint "step-2-radio-start"
    echo "2/3: Restarting the Wi-Fi radio..."
    RADIO_OFF=1
    timeout 10 nmcli radio wifi off || true
    sleep 2
    if timeout 10 nmcli radio wifi on; then RADIO_OFF=0; fi
    sleep 2
    connect_wifi || true
    snapshot_after radio
    if healthy; then checkpoint "recovered"; return 0; fi

    checkpoint "step-3-usb-start"
    echo "3/3: Resetting the USB Wi-Fi driver connection..."
    if reset_usb; then
        connect_wifi || true
        snapshot_after usb
        if healthy; then checkpoint "recovered"; return 0; fi
    fi
    checkpoint "recovery-not-confirmed"
    echo "Recovery was not confirmed: Wi-Fi must have an IPv4 address and answer gateway pings."
    echo "Check router power and USB/power connections. Logs have been saved."
    echo "If needed, reinsert the Wi-Fi dongle and run this script again."
    return 1
}

main() {
    local mode="${1:-}" command logdir status owner
    case "$mode" in
        --help|-h) usage; return 0 ;;
        ""|--check) ;;
        *) usage >&2; return 2 ;;
    esac
    if (( $# > 1 )); then usage >&2; return 2; fi
    for command in nmcli ip ping timeout flock python3 readlink awk; do
        command -v "$command" >/dev/null || { echo "Missing command: $command"; return 1; }
    done
    if [[ "$mode" == --check ]]; then
        if ! nmcli -g connection.id connection show "$PROFILE" >/dev/null 2>&1; then
        echo "Saved Wi-Fi profile '$PROFILE' is unavailable. No settings changed."
        return 1
    fi
        echo "Saved profile: $PROFILE; interface: $IFACE"
        if identify_usb; then
            echo "USB interface: $USB_INTERFACE; driver: ${USB_DRIVER##*/}"
        else
            echo "USB interface unavailable; automatic USB reset would be skipped."
        fi
        healthy
        return $?
    fi
    if (( EUID != 0 )); then
        exec sudo -- "$BASE_DIR/wifi_fix.sh"
    fi
    exec 9>/run/lock/robot-wifi-fix.lock
    flock -n 9 || { echo "Another Wi-Fi recovery is running."; return 1; }
    logdir="$BASE_DIR/robot0/out/raspberry_pi/wifi_fix_$(date +%Y%m%d_%H%M%S)_$$"
    mkdir -p "$logdir" || return 1
    # Make resulting recovery logs accessible to the owner of this home directory.
    owner="$(stat -c %u:%g "$BASE_DIR")"
    chown "$owner" "$logdir"
    RECOVERY_LOGDIR="$logdir"
    export RECOVERY_LOGDIR
    touch "$logdir/recovery.log"
    chown "$owner" "$logdir/recovery.log"
    exec > >(durable_tee "$logdir/recovery.log") 2>&1
    checkpoint "capture-start"
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    echo "Wi-Fi recovery started: $(date -Is)"
    echo "Log directory: $logdir"
    if ! nmcli -g connection.id connection show "$PROFILE"; then
        echo "Saved Wi-Fi profile '$PROFILE' is unavailable. No settings changed."
        checkpoint "profile-unavailable"
        return 1
    fi
    echo "Saving evidence BEFORE resetting Wi-Fi (up to 120 seconds)..."
    if [[ -f "$BASE_DIR/wifi_diag.py" ]]; then
        if [[ -n "${SUDO_USER:-}" && "$SUDO_USER" != root ]]; then
            timeout -s INT -k 5 120 runuser -u "$SUDO_USER" -- python3 "$BASE_DIR/wifi_diag.py" --seconds 0 || true
        else
            timeout -s INT -k 5 120 python3 "$BASE_DIR/wifi_diag.py" --seconds 0 || true
        fi
    else
        echo "wifi_diag.py missing; saving kernel and network journals here."
        journalctl -b -k --no-pager > "$logdir/kernel-before.txt"
        journalctl -b -u NetworkManager -u wpa_supplicant --no-pager > "$logdir/network-before.txt"
    fi
    checkpoint "capture-finished"
    status=0
    recover || status=$?
    journalctl -b -u NetworkManager -u wpa_supplicant -n 100 --no-pager > "$logdir/network-after.txt"
    ip -br address
    chown -R "$owner" "$logdir"
    checkpoint "finished-exit-$status"
    echo "Finished: $(date -Is); exit status: $status"
    echo "Recovery log: $logdir/recovery.log"
    return "$status"
}

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
    main "$@"
fi
