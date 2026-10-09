#!/bin/bash
# Reviewed next-boot change for this specific Pi. No services are stopped.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
MARKER=/etc/cloud/cloud-init.disabled
STATE=/home/pi/plant_lineFolower/out/boot_optimization
OWNED="$STATE/cloud-init-disabled-by-this-script"
fail() { echo "Stopped: $*" >&2; exit 1; }
[[ "$(hostname)" == raspberrypi ]] || fail "This script is for raspberrypi only."
case "${1:---status}" in
  --status)
    if [[ -e "$MARKER" ]]; then echo "cloud-init is disabled for subsequent boots."; else echo "cloud-init is enabled."; fi
    exit 0 ;;
  --apply|--undo) [[ $EUID == 0 ]] || fail "Run with sudo." ;;
  *) echo "Usage: $0 [--status|--apply|--undo]"; exit 2 ;;
esac
mkdir -p "$STATE"
exec 9>"$STATE/.lock"
flock -n 9 || fail "Another optimization command is running."
if [[ "$1" == --undo ]]; then
  if [[ ! -f "$OWNED" ]]; then
    echo "No change owned by this script to undo."
    exit 0
  fi
  [[ ! -s "$MARKER" ]] || fail "Disable marker was modified; review manually."
  rm -f "$MARKER" "$OWNED"
  printf '%s restored cloud-init\n' "$(date -u --iso-8601=seconds)" >>"$STATE/actions.log"
  sync
  echo "Restored cloud-init for the next boot."
  exit 0
fi
if [[ -e "$MARKER" ]]; then
  echo "cloud-init is already disabled; no change."
  exit 0
fi
[[ -f /var/lib/cloud/instance/boot-finished ]] || fail "Provisioning has not completed."
[[ -s /etc/NetworkManager/system-connections/robot-wifi.nmconnection ]] || fail "Persistent robot Wi-Fi profile is missing."
grep -Eq '^autoconnect=true$' /etc/NetworkManager/system-connections/robot-wifi.nmconnection || fail "Wi-Fi autoconnect needs review."
grep -Eq 'config: *disabled' /etc/cloud/cloud.cfg.d/zz-robot-native-network.cfg || fail "Existing cloud-init network override is missing."
if find /etc/netplan -maxdepth 1 -type f \( -name '*.yaml' -o -name '*.yml' \) -print -quit | grep -q .; then
  fail "Netplan configuration changed; review before disabling provisioning."
fi
for directory in /var/lib/cloud/scripts/per-boot /var/lib/cloud/instance/scripts; do
  if [[ -d "$directory" ]] && find "$directory" -type f -print -quit | grep -q .; then
    fail "Cloud-init scripts exist in $directory; review their purpose first."
  fi
done
umask 022
touch "$OWNED"
if ! (set -o noclobber; : >"$MARKER"); then
  rm -f "$OWNED"
  fail "Could not create cloud-init disable marker."
fi
printf '%s disabled cloud-init; other services unchanged\n' "$(date -u --iso-8601=seconds)" >>"$STATE/actions.log"
sync
echo "cloud-init disabled for NEXT BOOT. Current network remains connected."
echo "Undo: sudo /home/pi/plant_lineFolower/config/boot_optimize.sh --undo"
