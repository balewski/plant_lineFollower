#!/bin/bash
# Startup only: the controller and all sensor/motor processing remain C++.
set -euo pipefail
read -r client client_port server server_port extra <<< "${SSH_CONNECTION:-}"
if [[ -z "${client:-}" || -z "${server:-}" || -z "${server_port:-}" || -n "${extra:-}" || "$client" == *:* || "$server" == *:* ]]; then
    echo "Start this launcher over IPv4 SSH from the Desktop that will run the operator." >&2
    exit 2
fi
for arg in "$@"; do
    if [[ "$arg" == --bind || "$arg" == --peer ]]; then
        echo "This launcher derives --bind/--peer from SSH; use ./controller for manual addresses." >&2
        exit 2
    fi
done
root=/home/pi/plant_lineFollower
cd -- "$root/lineFollowerDistributedBin"
printf 'SSH route: Pi %s; Desktop %s (pinned for this process).\n' "$server" "$client"
exec ./controller "$@" --bind "$server" --peer "$client"
