#!/bin/sh
# Run on the Pi. Password is entered privately on its terminal.
exec sudo /usr/bin/python3 - <<'PY'
import getpass
import os
from pathlib import Path
import subprocess
import tempfile
import yaml

ssid = 'chatka pochatka'
password = getpass.getpass(f'Wi-Fi password for {ssid}: ')
if not (8 <= len(password) <= 63 or
        (len(password) == 64 and all(c in '0123456789abcdefABCDEF' for c in password))):
    raise SystemExit('Expected an 8–63 character password or a 64-digit hexadecimal key.')
config = {'network': {'version': 2, 'wifis': {'wlan0': {
    'renderer': 'NetworkManager', 'dhcp4': True, 'optional': True,
    'access-points': {ssid: {'password': password}}
}}}}
target = Path('/etc/netplan/80-robot-wifi.yaml')
if target.exists():
    raise SystemExit(f'{target} already exists; leaving it untouched for review.')
target.parent.mkdir(parents=True, exist_ok=True)
fd, temporary = tempfile.mkstemp(prefix='.robot-wifi-', dir=target.parent)
try:
    with os.fdopen(fd, 'w') as stream:
        yaml.safe_dump(config, stream, sort_keys=False)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, target)
finally:
    if os.path.exists(temporary):
        os.unlink(temporary)
os.sync()
print('Wi-Fi configuration saved with root-only permissions. Generating network settings...', flush=True)
subprocess.run(['/usr/sbin/netplan', 'generate'], check=True)
subprocess.run(['/usr/bin/nmcli', 'connection', 'reload'], check=True)
subprocess.run(['/usr/bin/nmcli', '--wait', '120', 'connection', 'up',
                'netplan-wlan0-' + ssid], check=True)
subprocess.run(['/usr/bin/nmcli', '-g', 'GENERAL.STATE,IP4.ADDRESS',
                'device', 'show', 'wlan0'], check=True)
print('Wi-Fi connected. Keep Ethernet plugged in until reboot persistence is verified.')
PY
