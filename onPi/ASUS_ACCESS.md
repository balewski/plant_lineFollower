# What the Pi may reach through the ASUS

Recorded 2026-10-04.

The Pi (`wlan0`, `10.0.0.149`) is on the ASUS at `10.0.0.1`, network `10.0.0.0/24`. That path is for OS update and `git clone`. It is not a general internet connection.

## Allowed

Outbound:

- This LAN, `10.0.0.0/24`, including DHCP and mDNS.
- DNS to the ASUS only: `10.0.0.1` UDP/TCP port 53.
- OS update, HTTP port 80 only, to the archives in this image:
  - `raspbian.raspberrypi.com` (`/etc/apt/sources.list.d/raspbian.sources`)
  - `archive.raspberrypi.com` (`/etc/apt/sources.list.d/raspi.sources`)
- `git clone` to GitHub only, TCP ports 22 and 443, using GitHub's published git and web address ranges.

Inbound:

- SSH, TCP port 22, from `10.0.0.0/24`.
- Ping and mDNS from `10.0.0.0/24`.
- DHCP replies, and replies to connections the Pi itself opened.

## Not allowed

Any other internet destination. Any incoming connection from outside `10.0.0.0/24`.

`apt` and `git clone` can still download code from those two archives and from GitHub. That is the accepted exception. A download from any other site is outside this policy.

## Status

On 2026-10-04 the ASUS hands the Pi no default route, and its DNS answers every name with `10.0.0.1`, so the Pi has no internet path. A matching packet filter is drafted in `onPi/firewall/nftables.conf`. It is not installed or enabled on the Pi.
