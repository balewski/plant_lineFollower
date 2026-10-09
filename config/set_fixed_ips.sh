#!/bin/bash
set -euo pipefail

echo "Configuring fixed IPs on Raspberry Pi..."

# Configure Wi-Fi to static IP 192.168.68.126
nmcli connection modify "robot-wifi" \
    ipv4.method manual \
    ipv4.addresses 192.168.68.126/24 \
    ipv4.gateway 192.168.68.1 \
    ipv4.dns "192.168.68.1 8.8.8.8" \
    ipv4.route-metric 600

# Configure Ethernet to static IP 192.168.68.125
nmcli connection modify "Wired connection 1" \
    ipv4.method manual \
    ipv4.addresses 192.168.68.125/24 \
    ipv4.gateway 192.168.68.1 \
    ipv4.dns "192.168.68.1 8.8.8.8" \
    ipv4.route-metric 100

nmcli connection reload

echo ""
echo "=== Successfully configured fixed IPs ==="
echo "Wi-Fi (wlan0):    192.168.68.126"
echo "Ethernet (eth0):  192.168.68.125"
echo "Gateway / DNS:    192.168.68.1"

