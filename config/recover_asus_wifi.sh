#!/usr/bin/env bash
set -euo pipefail
if [[ $EUID -ne 0 ]]; then
  echo 'Run this script with sudo.' >&2
  exit 1
fi
if ! ip -4 -o addr show dev eth0 | grep -q '192.168.68.'; then
  echo 'Home Ethernet must be connected before resetting Wi-Fi.' >&2
  exit 1
fi
printf 'Resetting only the RTL8192CU Wi-Fi adapter; Ethernet stays connected.\n'
trap 'nmcli radio wifi on >/dev/null 2>&1 || true' EXIT
nmcli radio wifi off
modprobe -r rtl8192cu
modprobe rtl8192cu
nmcli radio wifi on
# Driver reload returns before NetworkManager/wpa_supplicant necessarily has
# a usable device. Wait for readiness instead of racing its initialization.
export LC_ALL=C
printf 'Waiting for NetworkManager to detect the Wi-Fi adapter...\n'
recovery_deadline=$((SECONDS + 30))
while true; do
  recovery_state=$(nmcli -g GENERAL.STATE device show wlan0 2>/dev/null || true)
  recovery_state=${recovery_state%% *}
  case "$recovery_state" in
    30|40|50|60|70|80|90|100|120) break ;;
  esac
  if (( SECONDS >= recovery_deadline )); then
    echo 'Wi-Fi adapter did not become available within 30 seconds.' >&2
    nmcli device status >&2
    exit 1
  fi
  sleep 1
done
if [[ "$recovery_state" == 100 ]] &&
   [[ "$(nmcli -g GENERAL.CONNECTION device show wlan0)" == asus-robot ]]; then
  echo 'ASUS Wi-Fi already reconnected automatically.'
else
  nmcli --wait 35 connection up asus-robot ifname wlan0
fi
ip -br addr show wlan0
/usr/sbin/iw dev wlan0 link
/usr/sbin/iw dev wlan0 get power_save
