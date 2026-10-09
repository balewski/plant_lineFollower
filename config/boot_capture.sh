#!/bin/bash
# Collect existing boot records after startup; never starts robot hardware.
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
export LC_ALL=C SYSTEMD_PAGER=cat SYSTEMD_COLORS=0
umask 077
BASE=/home/pi/plant_lineFolower/out/boot_logs
mkdir -p "$BASE"
exec 9>"$BASE/.capture.lock"
flock -n 9 || exit 0
case "${1:-}" in
  --after-boot)
    # Let SSH and the normal boot finish before doing diagnostic I/O.
    sleep 60
    deadline=$((SECONDS + 600))
    while (( SECONDS < deadline )); do
      finished=$(timeout 15 systemctl show -p FinishTimestampMonotonic --value 2>/dev/null || true)
      if [[ "$finished" =~ ^[0-9]+$ ]] && [[ "$finished" != 0 ]]; then break; fi
      sleep 10
    done
    ;;
  "") ;;
  *) echo "Usage: $0 [--after-boot]" >&2; exit 2 ;;
esac
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)
OUT=$(mktemp -d "$BASE/${BOOT_ID}_$(date -u +%Y%m%dT%H%M%SZ)_XXXXXX")
capture() {
  local file="$1"; shift
  timeout 120 "$@" >"$OUT/$file" 2>&1
  local status=$?
  printf '%s exit=%s\n' "$file" "$status" >>"$OUT/collection-status.txt"
}
{
  echo "boot_id=$BOOT_ID"
  date -u --iso-8601=seconds
  echo "uptime_seconds=$(cut -d' ' -f1 /proc/uptime)"
  echo "Readiness target: SSH accessible over Wi-Fi."
  echo "Linux monotonic timestamps exclude firmware time before the kernel."
  echo "This collector runs after startup; it does not measure power application."
  uname -a
  tr '\0' '\n' </proc/device-tree/model
  cat /etc/os-release
} >"$OUT/metadata.txt"
capture time.txt systemd-analyze time
capture critical-chain.txt systemd-analyze critical-chain ssh.service NetworkManager.service multi-user.target
capture blame.txt systemd-analyze blame --no-pager
capture boot.svg systemd-analyze plot
# Limit manual late captures to prevent unexpectedly huge archives.
capture journal.txt journalctl -b -o short-monotonic --no-pager -n 20000
capture kernel.txt journalctl -b -k -o short-monotonic --no-pager
capture warnings.txt journalctl -b -p warning -o short-monotonic --no-pager
capture enabled-units.txt systemctl list-unit-files --state=enabled --no-pager
capture failed-units.txt systemctl --failed --no-pager
capture unit-timestamps.txt systemctl show ssh.service NetworkManager.service NetworkManager-wait-online.service cloud-init-main.service cloud-init-local.service cloud-init-network.service rpi-resize-swap-file.service multi-user.target -p Id -p ActiveState -p SubState -p ExecMainStartTimestampMonotonic -p ActiveEnterTimestampMonotonic -p InactiveExitTimestampMonotonic
capture network.txt nmcli -f GENERAL,IP4 device show
capture addresses.txt ip -brief address
capture memory.txt free -m
capture filesystems.txt df -h
capture throttled.txt vcgencmd get_throttled
capture temperature.txt vcgencmd measure_temp
capture processes.txt ps -eo pid,comm,etimes,time,pcpu,pmem --sort=-pcpu
cp /proc/swaps "$OUT/swaps.txt"
cp /proc/cmdline "$OUT/kernel-command-line.txt"
{
  cat "$OUT/time.txt"
  echo
  echo "SSH/Wi-Fi boot events (seconds since Linux kernel start):"
  grep -E 'Server listening on|Started ssh.service|dhcp4 .*new lease|Activation: successful, device activated|Startup finished in|startup complete' "$OUT/journal.txt" || true
  echo
  echo "Collection command status:"
  cat "$OUT/collection-status.txt"
} >"$OUT/summary.txt"
touch "$OUT/COMPLETE"
ln -sfn "$(basename "$OUT")" "$BASE/latest"
echo "$OUT"
