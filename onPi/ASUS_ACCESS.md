# What the Pi may reach through the ASUS

Recorded 2026-10-04.

The Pi (`wlan0`, `10.0.0.149`) is on the ASUS at `10.0.0.1`, network `10.0.0.0/24`.

`/home/pi/plant_lineFolower/config/asus_wan_route.sh` checks whether the ASUS can reach the public internet.

- When that check succeeds, the Pi uses `10.0.0.1` as its default gateway.
- When the check fails, Wi-Fi keeps only `10.0.0.0/24`. There is no default route.

`configure_wifi.sh`, `wifi_fix.sh`, and `wifi_diag.py` stay on the Pi in `/home/pi/plant_lineFolower/config/`.

A tighter packet filter is drafted in `onPi/firewall/nftables.conf`. It is not installed on the Pi.
