# Configuration & Diagnostic Scripts

This directory contains network configuration, boot optimization, and diagnostic scripts for both the host Mac and the Raspberry Pi (`plant_lineFollower`).

All scripts can be committed to GitHub and synced between the Mac and the Pi at `/home/pi/plant_lineFollower/config`.

---

## Network & Wi-Fi Management

### `set_fixed_ips.sh`
Configures permanent, fixed static IPs on the Raspberry Pi. `select_pi_ethernet.sh` chooses the Ethernet profile from which router answers:
- **A, Wi-Fi (`asus-robot`):** `10.0.0.20/24`, `ssh pi2`. Robot runs use this path. Unplug the Ethernet cable.
- **B, Ethernet to ASUS (`asus-ethernet`):** `10.0.0.120/24`, `ssh pi2-asus-eth`
- **C, Ethernet to the home router (`Wired connection 1`):** `192.168.68.220/24`, `ssh pi2-home-eth`, gateway and DNS `192.168.68.1`
- **Spare home Wi-Fi (`robot-wifi`):** `192.168.68.126/24`. Not one of paths A, B, or C.

Usage:
```bash
sudo bash set_fixed_ips.sh
```

### `wifi_diag.py`
Comprehensive Python diagnostic utility that checks Wi-Fi link status, signal strength, carrier rate, NetworkManager state, routing table, and internet connectivity.

Usage:
```bash
python3 wifi_diag.py
```

### `wifi_fix.sh`
Diagnostic and repair script for Wi-Fi on the Pi. Inspects interface state, re-enables Wi-Fi radio, and triggers reconnection to known networks.

Usage:
```bash
bash wifi_fix.sh
```

### `configure_wifi.sh`
Helper script to configure Wi-Fi SSID and passphrases in NetworkManager on the Pi.

Usage:
```bash
sudo bash configure_wifi.sh "<SSID>" "<PASSWORD>"
```

### `setup_maintenance_ethernet.sh`
Configures direct point-to-point fallback Ethernet (`192.168.77.2/24` on Pi, `192.168.77.1/24` on Mac) if DHCP fails after 15 seconds.

Usage:
```bash
sudo bash setup_maintenance_ethernet.sh
```

### `recover_asus_wifi.sh`
Reloads the RTL8192CU driver and reconnects to the ASUS Wi-Fi network without interrupting an active home Ethernet session.

Usage:
```bash
sudo bash recover_asus_wifi.sh
```

### `asus_wan_route.sh`
Configures routing and gateway policies between the Mac, ASUS router, and external interfaces.

---

## Boot stamp

`write_boot_stamp.sh` runs once per boot and writes `config/boot.json`. `hash8` is eight hex characters and changes only when the Pi reboots. `uptime_s` is seconds since that boot from `/proc/uptime`, so a later reading of `/proc/uptime` minus this value is the elapsed time while `hash8` still matches. `local_time` is the Pi's own clock at the moment the file was written. No network time is required.

`photo3_calib.json` and `clock_calib.json` in this directory copy that `hash8` and add their own `local_time` and `uptime_s`. A clock file whose `hash8` differs from the current `boot.json` was taken before a reboot. `photo3_calib.json` stores the printed sensor summary (mean, standard deviation, count, validity, polarity) and does not store individual ADC readings. `clock_calib.json` stores the printed clock summary (round trip and half-width on each path, and the Pi–Teensy offset, drift and residual range) and does not store individual probes or edges.

`boot-stamp.service` starts the script at boot. Install it on the Pi with:

```bash
sudo cp /home/pi/plant_lineFollower/config/boot-stamp.service /etc/systemd/system/
sudo systemctl enable --now boot-stamp.service
```

## Boot & System Tuning

### `boot_capture.sh`
Diagnostic tool that runs after boot to capture `systemd-analyze` critical chain, blame logs, journal boot events, and time-to-SSH metrics. Outputs logs to `out/boot_logs/`.

Usage:
```bash
bash boot_capture.sh [--after-boot]
```

### `boot_fixed_swap.sh`
Pins swap configuration (426 MiB zram + 426 MiB disk backing) to prevent repeated dynamic swap recalculation on boot.

Usage:
```bash
sudo bash boot_fixed_swap.sh --apply
sudo bash boot_fixed_swap.sh --undo
```

### `boot_optimize.sh`
Disables `cloud-init` on subsequent boots after initial provisioning is verified, speeding up boot time.

Usage:
```bash
sudo bash boot_optimize.sh --apply
sudo bash boot_optimize.sh --undo
```

