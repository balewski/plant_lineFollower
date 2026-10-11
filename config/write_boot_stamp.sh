#!/bin/bash
# Write config/boot.json once per boot. A later run in the same boot keeps hash8.
# uptime_s is seconds since this boot from /proc/uptime. It needs no network clock.
set -euo pipefail
dest=/home/pi/plant_lineFollower/config/boot.json
mkdir -p "$(dirname "$dest")"
boot_id=$(tr -d ' \n' < /proc/sys/kernel/random/boot_id)
hash8=$(printf '%s' "$boot_id" | sha256sum | awk '{print substr($1,1,8)}')
if [[ -f "$dest" ]]; then
    current=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("hash8",""))' "$dest")
    if [[ "$current" == "$hash8" ]]; then
        exit 0
    fi
fi
uptime=$(awk '{print $1}' /proc/uptime)
local_time=$(date +%Y-%m-%dT%H:%M:%S%z)
python3 - "$dest" "$hash8" "$local_time" "$uptime" << 'PY'
import json, os, sys
dest, hash8, local_time, uptime = sys.argv[1:]
body = {"hash8": hash8, "local_time": local_time, "uptime_s": float(uptime)}
tmp = dest + ".tmp"
with open(tmp, "w") as handle:
    json.dump(body, handle, indent=2)
    handle.write("\n")
    handle.flush()
    os.fsync(handle.fileno())
os.replace(tmp, dest)
print("boot stamp hash8=%s uptime_s=%s local_time=%s" % (hash8, uptime, local_time))
print(dest)
PY
