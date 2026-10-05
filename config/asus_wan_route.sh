#!/bin/bash
# Use the ASUS as the default gateway only while it can reach the public internet.
# Otherwise keep Wi-Fi on the local LAN (10.0.0.0/24) with no default route.
# Each run appends to wan_route.log in this directory.
set -u
export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"

IFACE=wlan0
GW=10.0.0.1
BASE=/home/pi/plant_lineFolower
SCRIPT=$BASE/config/asus_wan_route.sh
LOG=$BASE/wan_route.log

note() {
    printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" >> "$LOG"
    echo "$*"
}

snapshot() {
    note "link: $(ip -br link 2>/dev/null | tr '\n' ';')"
    note "addr: $(ip -4 -br addr 2>/dev/null | tr '\n' ';')"
    note "route: $(ip route 2>/dev/null | tr '\n' ';')"
    note "nm: $(nmcli -t -f DEVICE,STATE,CONNECTION device 2>/dev/null | tr '\n' ';')"
}

finish() {
    chmod 644 "$LOG" 2>/dev/null || true
    sync
}

install_units() {
    cat > /etc/systemd/system/asus-wan-route.service << 'UNIT'
[Unit]
Description=Use the ASUS gateway only while it can reach the internet
After=network-online.target

[Service]
Type=oneshot
ExecStart=/home/pi/plant_lineFolower/config/asus_wan_route.sh
UNIT
    cat > /etc/systemd/system/asus-wan-route.timer << 'UNIT'
[Unit]
Description=Recheck whether the ASUS can reach the internet

[Timer]
OnBootSec=20
OnUnitActiveSec=30
AccuracySec=5s

[Install]
WantedBy=timers.target
UNIT
    cat > /etc/NetworkManager/dispatcher.d/50-asus-wan-route << 'UNIT'
#!/bin/sh
[ "$1" = "wlan0" ] || exit 0
case "$2" in
    up|dhcp4-change) /home/pi/plant_lineFolower/config/asus_wan_route.sh ;;
esac
UNIT
    chmod 755 /etc/NetworkManager/dispatcher.d/50-asus-wan-route
    systemctl daemon-reload
    systemctl enable --now asus-wan-route.timer
    echo "Installed asus-wan-route.timer."
}

if [ "${1:-}" = "--install" ]; then
    if [ "$(id -u)" -ne 0 ]; then
        exec sudo -- "$SCRIPT" --install
    fi
    install_units
    exec "$SCRIPT"
fi

if [ "$(id -u)" -ne 0 ]; then
    echo "This script changes routes. Run: sudo $SCRIPT"
    exit 1
fi

exec 9>/run/lock/asus-wan-route.lock
if ! flock -n 9; then
    note "asus wan: another check is running."
    finish
    exit 0
fi

apply_profile() {
    local want="$1" profile current
    profile="$(nmcli -g GENERAL.CONNECTION device show "$IFACE" 2>/dev/null || true)"
    [ -n "$profile" ] && [ "$profile" != "--" ] || return 0
    current="$(nmcli -g ipv4.never-default connection show "$profile" 2>/dev/null || true)"
    [ "$current" = "$want" ] && return 0
    nmcli connection modify "$profile" ipv4.never-default "$want"
    nmcli device reapply "$IFACE"
}

snapshot
if ! ip -4 addr show dev "$IFACE" 2>/dev/null | grep -q 'inet '; then
    linkline="$(ip -br link show "$IFACE" 2>/dev/null || true)"
    if [[ "$linkline" == *NO-CARRIER* || "$linkline" == *" DOWN "* ]]; then
        note "asus wan: $IFACE lost carrier; bringing asus-robot up."
        if timeout 40 nmcli --wait 30 connection up id asus-robot ifname "$IFACE"; then
            note "asus wan: asus-robot is up."
        else
            note "asus wan: bringing asus-robot up failed."
        fi
        snapshot
    else
        note "asus wan: $IFACE has no IPv4 address; routes unchanged."
    fi
    finish
    exit 0
fi

if ! ping -n -I "$IFACE" -c 3 -W 2 "$GW" >/dev/null 2>&1; then
    ip route del default via "$GW" dev "$IFACE" 2>/dev/null || true
    apply_profile yes
    note "asus wan: ASUS $GW does not answer; local route only."
    snapshot
    finish
    exit 0
fi

ip route replace default via "$GW" dev "$IFACE" metric 600
probe_log="$(python3 -c '
import socket, sys
for dest in (("1.1.1.1", 443), ("8.8.8.8", 443), ("9.9.9.9", 443)):
    try:
        s = socket.create_connection(dest, 3)
        s.close()
        print("probe %s:%s ok" % dest)
        sys.exit(0)
    except OSError as exc:
        print("probe %s:%s %s" % (dest[0], dest[1], exc))
sys.exit(1)
')" && probe_ok=1 || probe_ok=0
printf '%s\n' "$probe_log" | while IFS= read -r line; do
    [ -n "$line" ] && note "$line"
done

if [ "$probe_ok" -eq 1 ]; then
    apply_profile no
    note "asus wan: internet reachable; default via $GW dev $IFACE."
    snapshot
    finish
    exit 0
fi

ip route del default via "$GW" dev "$IFACE" 2>/dev/null || true
apply_profile yes
note "asus wan: internet unreachable; local route only."
snapshot
finish
