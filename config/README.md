# Configuration & Diagnostic Scripts

This directory contains network configuration, boot optimization, and diagnostic scripts for both the host Mac and the Raspberry Pi (`plant_lineFolower`).

All scripts can be committed to GitHub and synced between the Mac and the Pi at `/home/pi/plant_lineFolower/config`.

---

## Network & Wi-Fi Management

### `set_fixed_ips.sh`
Configures permanent, fixed static IPs on the Raspberry Pi:
- **Wi-Fi (`wlan0`):** `192.168.68.126/24` (Metric: 600)
- **Ethernet (`eth0`):** `192.168.68.125/24` (Metric: 100)
- **Gateway & DNS:** `192.168.68.1` (Home Router)

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

