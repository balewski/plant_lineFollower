#!/bin/bash
# Choose SSH path A, B, or C from the Ethernet cable on eth0.
#   A  no cable     Wi-Fi only          10.0.0.149
#   B  cable→ASUS   Ethernet            10.0.0.24
#   C  cable→home   Ethernet            192.168.68.125
# The ASUS ignores ARP from a foreign source address, so each probe is sent
# from the fixed address of that network.
set -u
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

IFACE=eth0
LOCK=/run/select-pi-ethernet.lock

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

exec 9>"$LOCK"
if ! flock -n 9; then
    echo "Ethernet selector already running."
    exit 0
fi

ensure_path_a() {
    local wifi
    wifi="$(nmcli -g GENERAL.CONNECTION device show wlan0 2>/dev/null || true)"
    if [[ $wifi == "asus-robot" ]]; then
        echo "Path A: no Ethernet cable. Wi-Fi already 10.0.0.149."
        return 0
    fi
    echo "Path A: no Ethernet cable. Wi-Fi only, 10.0.0.149."
    nmcli connection up asus-robot
}

if ! ip link show "$IFACE" 2>/dev/null | grep -q "LOWER_UP"; then
    ensure_path_a
    exit 0
fi

probe() {
    local addr="$1" gw="$2" added=0
    if ! ip -4 -o addr show dev "$IFACE" | grep -q "inet ${addr}/"; then
        ip addr add "${addr}/24" dev "$IFACE"
        added=1
    fi
    ping -c 1 -W 1 -I "$addr" "$gw" >/dev/null 2>&1
    local ok=$?
    if [[ $added -eq 1 ]]; then
        ip addr del "${addr}/24" dev "$IFACE" 2>/dev/null || true
    fi
    return "$ok"
}

asus=0
home=0
probe 10.0.0.24 10.0.0.1 && asus=1
probe 192.168.68.125 192.168.68.1 && home=1
current="$(nmcli -g GENERAL.CONNECTION device show "$IFACE" 2>/dev/null || true)"

if [[ $asus -eq 1 && $home -eq 1 ]]; then
    echo "Both 10.0.0.1 and 192.168.68.1 answered on eth0; Ethernet profile unchanged ($current)."
    exit 0
fi

if [[ $asus -eq 1 ]]; then
    if [[ $current == "asus-ethernet" ]]; then
        echo "Path B: Ethernet cable to ASUS. Already 10.0.0.24."
        exit 0
    fi
    echo "Path B: Ethernet cable to ASUS. Switching eth0 to 10.0.0.24."
    nmcli connection up asus-ethernet
    exit 0
fi

if [[ $home -eq 1 ]]; then
    if [[ $current == "Wired connection 1" ]]; then
        echo "Path C: Ethernet cable to the home router. Already 192.168.68.125."
        exit 0
    fi
    echo "Path C: Ethernet cable to the home router. Switching eth0 to 192.168.68.125."
    nmcli connection up "Wired connection 1"
    exit 0
fi

echo "Ethernet cable is up but neither router answered. Leave $current."
exit 0
