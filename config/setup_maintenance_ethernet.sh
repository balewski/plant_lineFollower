#!/bin/bash
# Prepare automatic direct-cable fallback; preserve the active DHCP connection.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
if [[ "${1:-}" == "--help" ]]; then
    echo "Usage: sudo bash $0"
    echo "Backs up NetworkManager profiles; adds 192.168.77.2/24 Ethernet fallback."
    echo "Keeps DHCP preferred; tries DHCP once for 15 seconds before fallback."
    echo "Does not reactivate interfaces or change Wi-Fi, SSH, gateway, or DNS."
    exit 0
fi
[[ $# == 0 ]] || { echo "Unexpected arguments" >&2; exit 2; }
[[ $EUID == 0 ]] || { echo "Run with sudo in an interactive terminal." >&2; exit 1; }
[[ $(hostname) == raspberrypi ]] || { echo "Unexpected host" >&2; exit 1; }
wired_uuid=0a9fb68a-407a-3937-8a5e-d96b1174ce41
fallback=robot-direct-ethernet
[[ $(nmcli -g ipv4.method connection show uuid "$wired_uuid") == auto ]] || {
    echo "Expected wired DHCP profile is absent or changed." >&2; exit 1;
}
[[ $(nmcli -g GENERAL.CON-UUID device show eth0) == "$wired_uuid" ]] || {
    echo "Expected wired DHCP connection must be active." >&2; exit 1;
}
systemctl is-active --quiet ssh || { echo "SSH is not running." >&2; exit 1; }
if nmcli -g connection.uuid connection show id "$fallback" >/dev/null 2>&1; then
    echo "Maintenance profile already exists; inspect it before rerunning." >&2
    exit 1
fi
backup=/home/pi/plant_lineFolower/out/maintenance_setup_$(date +%Y%m%d_%H%M%S)
umask 077
mkdir -p "$backup"
cp -a /etc/NetworkManager/system-connections "$backup/"
nmcli -f connection,ipv4,ipv6 connection show uuid "$wired_uuid" > "$backup/wired_before.txt"
ip -brief address > "$backup/addresses_before.txt"
ip route > "$backup/routes_before.txt"
# This profile is tried only after the preferred DHCP profile fails.
nmcli connection add type ethernet ifname eth0 con-name "$fallback" \
    connection.autoconnect yes connection.autoconnect-priority -900 \
    ipv4.method manual ipv4.addresses 192.168.77.2/24 \
    ipv4.never-default yes ipv6.method disabled
# Saving these changes does not disconnect the currently active wired session.
nmcli connection modify uuid "$wired_uuid" \
    connection.autoconnect yes connection.autoconnect-priority 100 \
    connection.autoconnect-retries 1 ipv4.dhcp-timeout 15
nmcli -f connection,ipv4,ipv6 connection show id "$fallback" > "$backup/fallback_after.txt"
nmcli -f connection,ipv4 connection show uuid "$wired_uuid" > "$backup/wired_after.txt"
printf 'Prepared Ethernet maintenance fallback. Backup: %s\n' "$backup"
echo "Current connection is unchanged. Test after moving the cable to the Mac."
echo "Mac: 192.168.77.1/24, no router or DNS. Pi: 192.168.77.2/24."
echo "Allow about 15-30 seconds for DHCP to fail, then SSH to pi@192.168.77.2."
echo "For rollback, restore the backed-up connection files and reload NetworkManager."
