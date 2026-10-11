#!/bin/bash
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "Run with sudo: sudo $0" >&2
    exit 1
fi

echo "Configuring fixed IPs on Raspberry Pi..."

# Home Wi-Fi is not one of paths A/B/C. Keep it available at a lower priority.
nmcli connection modify "robot-wifi" \
    connection.autoconnect yes \
    connection.autoconnect-priority 10 \
    ipv4.method manual \
    ipv4.addresses 192.168.68.126/24 \
    ipv4.gateway 192.168.68.1 \
    ipv4.dns "192.168.68.1 8.8.8.8" \
    ipv4.route-metric 600

# Path A. Preferred Wi-Fi, used when the Ethernet cable is unplugged.
nmcli connection modify "asus-robot" \
    connection.autoconnect yes \
    connection.autoconnect-priority 200 \
    ipv4.method manual \
    ipv4.addresses 10.0.0.20/24 \
    ipv4.gateway "" \
    ipv4.dns "" \
    ipv4.never-default yes \
    ipv4.route-metric 600

# Home Ethernet. Higher autoconnect priority, so it is tried first after boot.
# select_pi_ethernet.sh moves to the ASUS profile when 10.0.0.1 answers.
nmcli connection modify "Wired connection 1" \
    connection.interface-name eth0 \
    connection.autoconnect yes \
    connection.autoconnect-priority 100 \
    ipv4.method manual \
    ipv4.addresses 192.168.68.220/24 \
    ipv4.gateway 192.168.68.1 \
    ipv4.dns "192.168.68.1 8.8.8.8" \
    ipv4.route-metric 100

# ASUS Ethernet. Saved, not forced up while the home router still answers.
if nmcli -g connection.id connection show "asus-ethernet" >/dev/null 2>&1; then
    nmcli connection modify "asus-ethernet" \
        connection.interface-name eth0 \
        connection.autoconnect yes \
        connection.autoconnect-priority 90 \
        ipv4.method manual \
        ipv4.addresses 10.0.0.120/24 \
        ipv4.gateway "" \
        ipv4.dns "" \
        ipv4.never-default yes \
        ipv4.route-metric 200 \
        ipv6.method disabled
else
    nmcli connection add type ethernet ifname eth0 con-name "asus-ethernet" \
        connection.autoconnect no \
        connection.autoconnect-priority 90 \
        ipv4.method manual \
        ipv4.addresses 10.0.0.120/24 \
        ipv4.never-default yes \
        ipv4.route-metric 200 \
        ipv6.method disabled
    nmcli connection modify "asus-ethernet" connection.autoconnect yes
fi

# Direct Desktop cable. Already fixed; reaffirm the address only.
nmcli connection modify "robot-direct-ethernet" \
    connection.interface-name eth0 \
    ipv4.method manual \
    ipv4.addresses 192.168.77.2/24 \
    ipv4.never-default yes \
    ipv6.method disabled

base="$(cd "$(dirname "$0")" && pwd)"
install -d /etc/NetworkManager/dispatcher.d
cat > /etc/NetworkManager/dispatcher.d/50-select-pi-ethernet << EOF
#!/bin/bash
[ "\$1" = eth0 ] || exit 0
case "\$2" in
    up|down|dhcp4-change) ;;
    *) exit 0 ;;
esac
exec ${base}/select_pi_ethernet.sh
EOF
chmod 755 /etc/NetworkManager/dispatcher.d/50-select-pi-ethernet
install -m 644 "$base/select-pi-ethernet.service" /etc/systemd/system/select-pi-ethernet.service
systemctl daemon-reload
systemctl enable select-pi-ethernet.service
nmcli connection reload

echo ""
echo "=== Successfully configured fixed IPs ==="
echo "Wi-Fi home   (robot-wifi):            192.168.68.126/24"
echo "Wi-Fi ASUS   (asus-robot):            10.0.0.20/24"
echo "Ethernet home (Wired connection 1):   192.168.68.220/24"
echo "Ethernet ASUS (asus-ethernet):        10.0.0.120/24"
echo "Ethernet direct (robot-direct-ethernet): 192.168.77.2/24"
echo "Gateway / DNS on the home profiles:   192.168.68.1"
echo ""
echo "Path A  no Ethernet cable              ssh pi2"
echo "Path B  Ethernet cable plugged into ASUS   ssh pi2-asus-eth"
echo "Path C  Ethernet cable plugged into home   ssh pi2-home-eth"
"$base/select_pi_ethernet.sh" || true
