#!/bin/bash
# Choose SSH path A, B, or C from the Ethernet cable on eth0.
#   A  no cable     Wi-Fi only          10.0.0.20
#   B  cable→ASUS   Ethernet            10.0.0.120
#   C  cable→home   Ethernet            192.168.68.220
# The ASUS ignores ARP from a foreign source address, so each probe is sent
# from the fixed address of that network.
# Each run appends to out/ethernet-select.log.
set -u
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

IFACE=eth0
LOCK=/run/select-pi-ethernet.lock
BASE=/home/pi/plant_lineFollower
LOG=$BASE/out/ethernet-select.log

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

mkdir -p -- "$BASE/out"

note() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG"
    echo "$*"
}

snapshot() {
    note "link: $(ip -br link 2>/dev/null | tr '\n' ';')"
    note "addr: $(ip -4 -br addr 2>/dev/null | tr '\n' ';')"
    note "route: $(ip route 2>/dev/null | tr '\n' ';')"
    note "nm: $(nmcli -t -f DEVICE,STATE,CONNECTION device 2>/dev/null | tr '\n' ';')"
    note "kernel: $(dmesg --ctime 2>/dev/null | grep smsc95xx | tail -n 1 || true)"
}

finish() {
    chmod 644 "$LOG" 2>/dev/null || true
    sync
}
trap finish EXIT

exec 9>"$LOCK"
if ! flock -n 9; then
    note "Ethernet selector already running."
    exit 0
fi

action="${NM_DISPATCHER_ACTION:-boot}"
note "select action=$action"

# USB Ethernet can report no carrier for a few seconds after boot.
# A down event means the cable was just removed, so do not wait.
if [[ "$action" != "down" ]]; then
    waited=0
    while ! ip link show "$IFACE" 2>/dev/null | grep -q "LOWER_UP"; do
        waited=$((waited + 1))
        [[ $waited -ge 8 ]] && break
        sleep 2
    done
    if [[ $waited -gt 0 ]]; then
        note "waited $((waited * 2))s for eth0 carrier"
    fi
fi

snapshot

announce() {
    local iface="$1" ip="$2"
    if python3 - "$iface" "$ip" << 'PY'
import fcntl, socket, struct, sys
iface, ip = sys.argv[1], sys.argv[2]
req = struct.pack("256s", iface[:15].encode())
info = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
mac = fcntl.ioctl(info.fileno(), 0x8927, req)[18:24]
info.close()
ipb = socket.inet_aton(ip)
raw = socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0806))
raw.bind((iface, 0))
for op in (1, 2):
    frame = b"\xff" * 6 + mac + struct.pack("!H", 0x0806)
    frame += struct.pack("!HHBBH", 1, 0x0800, 6, 4, op)
    frame += mac + ipb + b"\xff" * 6 + ipb
    raw.send(frame)
raw.close()
PY
    then
        note "announced $ip on $iface"
    else
        note "announce $ip on $iface failed"
    fi
}

announce_configured() {
    local line iface cidr ip
    while read -r line; do
        iface="${line%% *}"
        cidr="${line#* }"
        ip="${cidr%%/*}"
        [[ -n "$iface" && -n "$ip" ]] || continue
        announce "$iface" "$ip"
    done < <(ip -4 -o addr show scope global | awk '{print $2, $4}')
}

reach() {
    local src="$1" dst="$2" label="$3"
    if ! ip -4 -o addr show | grep -q "inet ${src}/"; then
        return 0
    fi
    if ping -c 1 -W 1 -I "$src" "$dst" >/dev/null 2>&1; then
        note "reach $label: $src -> $dst answered"
    else
        note "reach $label: $src -> $dst no answer"
    fi
}

report_paths() {
    announce_configured
    reach 10.0.0.20 10.0.0.2 "A"
    reach 10.0.0.120 10.0.0.2 "B"
    reach 192.168.68.220 192.168.68.107 "C"
    snapshot
    note "log: $LOG"
}

ensure_path_a() {
    local wifi
    wifi="$(nmcli -g GENERAL.CONNECTION device show wlan0 2>/dev/null || true)"
    if [[ $wifi == "asus-robot" ]]; then
        note "Path A: no Ethernet cable. Wi-Fi already 10.0.0.20."
        return 0
    fi
    note "Path A: no Ethernet cable. Wi-Fi only, 10.0.0.20."
    if nmcli connection up asus-robot; then
        note "Path A: asus-robot is up."
    else
        note "Path A: nmcli connection up asus-robot failed."
    fi
}

if ! ip link show "$IFACE" 2>/dev/null | grep -q "LOWER_UP"; then
    ensure_path_a
    report_paths
    exit 0
fi

probe() {
    local addr="$1" gw="$2" added=0 try=1
    if ! ip -4 -o addr show dev "$IFACE" | grep -q "inet ${addr}/"; then
        ip addr add "${addr}/24" dev "$IFACE"
        added=1
    fi
    local ok=1
    for try in 1 2 3; do
        if ping -c 1 -W 1 -I "$addr" "$gw" >/dev/null 2>&1; then
            ok=0
            break
        fi
    done
    if [[ $added -eq 1 ]]; then
        ip addr del "${addr}/24" dev "$IFACE" 2>/dev/null || true
    fi
    if [[ $ok -eq 0 ]]; then
        note "probe $addr -> $gw answered on try $try"
    else
        note "probe $addr -> $gw no answer"
    fi
    return "$ok"
}

asus=0
home=0
probe 10.0.0.120 10.0.0.1 && asus=1
probe 192.168.68.220 192.168.68.1 && home=1
current="$(nmcli -g GENERAL.CONNECTION device show "$IFACE" 2>/dev/null || true)"

if [[ $asus -eq 1 && $home -eq 1 ]]; then
    note "Both 10.0.0.1 and 192.168.68.1 answered on eth0; Ethernet profile unchanged ($current)."
    report_paths
    exit 0
fi

if [[ $asus -eq 1 ]]; then
    if [[ $current == "asus-ethernet" ]] && ip -4 -o addr show dev "$IFACE" | grep -q "inet 10.0.0.120/"; then
        note "Path B: Ethernet cable to ASUS. Already 10.0.0.120."
    else
        note "Path B: Ethernet cable to ASUS. Switching eth0 to 10.0.0.120."
        if nmcli connection up asus-ethernet; then
            note "Path B: asus-ethernet is up."
        else
            note "Path B: nmcli connection up asus-ethernet failed."
        fi
    fi
    report_paths
    exit 0
fi

if [[ $home -eq 1 ]]; then
    if [[ $current == "Wired connection 1" ]] && ip -4 -o addr show dev "$IFACE" | grep -q "inet 192.168.68.220/"; then
        note "Path C: Ethernet cable to the home router. Already 192.168.68.220."
    else
        note "Path C: Ethernet cable to the home router. Switching eth0 to 192.168.68.220."
        if nmcli connection up "Wired connection 1"; then
            note "Path C: Wired connection 1 is up."
        else
            note "Path C: nmcli connection up Wired connection 1 failed."
        fi
    fi
    report_paths
    exit 0
fi

note "Ethernet cable is up but neither router answered. Leave $current."
report_paths
exit 0
