#!/bin/bash
# Keep this Pi's existing swap capacity while avoiding repeated size calculation.
# Only writes a supported rpi-swap config drop-in; takes effect next boot.
set -euo pipefail
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
TARGET=/etc/rpi/swap.conf.d/90-robot-fixed-size.conf
STATE=/home/pi/plant_lineFolower/out/boot_tuning/fixed_swap_state
SIZE_MIB=426
SIZE_BYTES=446693376
fail() { echo "Stopped: $*" >&2; exit 1; }
config() {
  printf '%s\n' '# Robot Pi: retain current swap sizes; avoid automatic sizing at every generator run.' '[File]' 'FixedSizeMiB=426' '' '[Zram]' 'FixedSizeMiB=426'
}
[[ "$(hostname)" == raspberrypi ]] || fail "This script is prepared for raspberrypi only."
check() {
  [[ "$(tr -d '\0' </proc/device-tree/model)" == "Raspberry Pi Model B Plus Rev 1.2" ]] || fail "Pi model changed."
  [[ "$(stat -c %s /var/swap)" == "$SIZE_BYTES" ]] || fail "Disk backing size changed; review first."
  [[ "$(cat /sys/block/zram0/disksize)" == "$SIZE_BYTES" ]] || fail "Active zram capacity changed; review first."
  [[ "$(cat /sys/block/zram0/backing_dev)" == /dev/loop* ]] || fail "Expected zram with loop-device backing."
  local mechanism=auto backing=/var/swap file_size= zram_size=
  local settings
  settings=$(rpi-systemd-config rpi/swap.conf mechanism Main::Mechanism backing File::Path file_size File::FixedSizeMiB zram_size Zram::FixedSizeMiB)
  eval "$settings"
  [[ "$mechanism" == auto || "$mechanism" == zram+file ]] || fail "Swap mechanism changed."
  [[ "$backing" == /var/swap ]] || fail "Backing path changed."
  [[ -z "$file_size" || "$file_size" == "$SIZE_MIB" ]] || fail "Configured file size changed."
  [[ -z "$zram_size" || "$zram_size" == "$SIZE_MIB" ]] || fail "Configured zram size changed."
}
mode=${1:---check}
case "$mode" in
  --check) check; echo "Preflight passed: disk backing and zram are both 426 MiB."; config; exit 0 ;;
  --status)
    if [[ -f "$TARGET" ]]; then cat "$TARGET"; else echo "Fixed-size override is not installed."; fi
    echo "Active zram bytes: $(cat /sys/block/zram0/disksize)"
    echo "Active backing: $(cat /sys/block/zram0/backing_dev)"
    exit 0 ;;
  --apply|--undo) [[ $EUID == 0 ]] || fail "Run with sudo." ;;
  *) echo "Usage: $0 [--check|--status|--apply|--undo]"; exit 2 ;;
esac
mkdir -p "$STATE"
exec 9>"$STATE/.lock"
flock -n 9 || fail "Another fixed-swap command is running."
if [[ "$mode" == --undo ]]; then
  [[ -f "$STATE/installed.conf" ]] || fail "No installation owned by this script."
  if [[ -e "$TARGET" ]]; then
    cmp -s "$TARGET" "$STATE/installed.conf" || fail "Override changed since installation; review manually."
    rm "$TARGET"
  fi
  rm "$STATE/installed.conf"
  printf '%s removed fixed-size override\n' "$(date -u --iso-8601=seconds)" >>"$STATE/actions.log"
  sync
  echo "Automatic sizing restored for NEXT BOOT; active swap is unchanged."
  exit 0
fi
check
if [[ -e "$TARGET" ]]; then
  if [[ -f "$STATE/installed.conf" ]] && cmp -s "$TARGET" "$STATE/installed.conf"; then
    echo "Already installed. Effective after the next boot."
    exit 0
  fi
  fail "Target already exists; refusing to overwrite it."
fi
mkdir -p "$(dirname "$TARGET")"
umask 022
(set -o noclobber; config >"$TARGET")
# Validate precedence through the same parser used by the installed package.
committed=no
trap 'if [[ "$committed" != yes ]]; then rm -f "$TARGET"; fi' EXIT
file_size= zram_size=
settings=$(rpi-systemd-config rpi/swap.conf file_size File::FixedSizeMiB zram_size Zram::FixedSizeMiB)
eval "$settings"
[[ "$file_size" == "$SIZE_MIB" && "$zram_size" == "$SIZE_MIB" ]] || fail "Another drop-in overrides the requested sizes."
cp "$TARGET" "$STATE/installed.conf"
printf '%s fixed both swap sizes at 426 MiB; retained disk writeback\n' "$(date -u --iso-8601=seconds)" >>"$STATE/actions.log"
committed=yes
sync
echo "Installed for NEXT BOOT: 426 MiB zram plus the existing 426 MiB disk backing."
echo "Current swap and networking remain active. No daemon reload or resize was run."
echo "Undo: sudo /home/pi/plant_lineFolower/config/boot_fixed_swap.sh --undo"
