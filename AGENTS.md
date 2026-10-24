# Project Agent Rules and Guidelines

## 1. Git & Version Control Policy
- **NO `git commit` or `git push`**: Under no circumstances should the agent execute `git commit` or `git push`. The user handles all commits and pushes to remote repositories.
- **Platform Selection by Task Type (Single-Platform Modification & Staging Policy)**:
  - **DO NOT modify or `git add` the same file on both Mac and `pi2`.**
  - **Circuit / Hardware Execution Tasks &rarr; `pi2` Only**: If a task is related to running, testing, or calibrating hardware/circuits on the Raspberry Pi, modify files **only in the repo on `pi2`** (`/home/pi/plant_lineFollower/`).
  - **Documentation / TeX / CAD Tasks &rarr; Mac Only**: If a task is documentation-oriented (e.g., updating `.tex` or `.tikz` files in `docs/`, or CAD/slicing on Mac), modify files **only in the Mac repo** (`/Users/balewski/shared_volumes/2026_plant_lineFollower/`).
  - **Ambiguous Tasks &rarr; Ask the User**: If it is not clear whether changes should be made on Mac or on `pi2`, explicitly ask the user where the changes should be made before editing any files.
  - Depending on the chosen platform, update and stage files **only on the platform where the work is taking place**. Never stage or commit the same file in parallel on both machines; the user handles git synchronization between Mac and Pi.
- **Read-only Git Inspection**: Commands such as `git status`, `git diff`, and `git log` are permitted on both platforms.

---

## 2. Workspace & Directory Structure
- **Active Workspace**: `/Users/balewski/shared_volumes/2026_plant_lineFollower/`
  - Do NOT use `/Users/balewski/shared_volumes/2026_robot1`.
- **Configuration & Diagnostics**:
  - All configuration and diagnostic files/scripts are located in `config/` (both on the Mac and on the Raspberry Pi: `~/plant_lineFollower/config/`).
  - On the Raspberry Pi, do NOT place loose scripts or configuration files in `/home/pi/`. Keep `/home/pi/` clean.
- **Robot Code on Raspberry Pi**:
  - Robot runtime code and test scripts belong under `~/plant_lineFollower/onPi/` (e.g., `onPi/toys/`).
  - Scripts running on the Pi must be self-contained within `~/plant_lineFollower/onPi/` with no dependencies on legacy `~/old-robot0`.

---

## 3. Remote Raspberry Pi Environment
- **Host**: SSH as `pi2` with key `~/.ssh/pi2_robot_ed25519`. Hosts in `~/.ssh/config`: `pi2` `10.0.0.20`, `pi2-asus-eth` `10.0.0.120`, `pi2-home-eth` `192.168.68.220`.
- **Fixed IPs**:
  - A, Wi-Fi (`wlan0`, `asus-robot`): `10.0.0.20`. Robot runs use this path only.
  - B, Ethernet to ASUS (`eth0`, `asus-ethernet`): `10.0.0.120`
  - C, Ethernet to the home router (`eth0`, `Wired connection 1`): `192.168.68.220`, gateway / DNS `192.168.68.1`
  - Desktop: `10.0.0.2` on the ASUS, `192.168.68.107` on home Wi-Fi
- **UART / Hardware**:
  - Serial port on Pi: `/dev/serial0` (115200 baud, 8N1) connected to Teensy 4.1 companion firmware.

---

## 4. 3D Printing & CAD Rules (assembly_sep24)

### Printer & Tools Setup
- **Printer Model**: QIDI Tech Q2 (CoreXY, Klipper firmware with Moonraker web API)
- **Moonraker API Host**: `http://192.168.68.123:7125`
- **Build Volume**: 270 × 270 × 260 mm
- **Default Material**: PETG (Black)
- **Standard Temperatures**: Bed 80 °C, Hotend 245–250 °C (0.4 mm hardened nozzle)
- **Slicer Path (macOS)**: `/Applications/QIDIStudio.app/Contents/MacOS/QIDIStudio`
- **FreeCAD Python Runtime**: `/Applications/FreeCAD.app/Contents/Resources/bin/python` with `PYTHONPATH=/Applications/FreeCAD.app/Contents/Resources/lib`

### Mandatory Safety Policies
1. **User Approval Before Printing**:
   - **Never start a print job automatically.**
   - Before uploading or starting a print, provide the user with the proposed plate layout, layer orientation, and mechanical strength / layer bonding tradeoffs.
   - **Display As-Printed Layout PNG**: Always generate and display the layout diagram in the established as-printed style:
     - 270 × 270 mm bed rectangle with axes and clear front/rear markers (`FRONT OF BED (Y = 0)` vs `REAR OF BED (Y = 270)`).
     - First layer contact footprint (explicitly stating which face is flat against the bed at Z = 0).
     - Column/upper structure outlines at top height.
     - Color-coded locations of snug support pillars (if any) annotated with directional pointers to indicate what overhangs are supported.
     - Automatically display the PNG on the user's screen using the macOS `open` command (e.g. `open <path_to_layout>.png`).
   - Request and wait for explicit user confirmation that:
     1. The layout and orientation are approved.
     2. The print bed is empty and clean.
     3. PETG filament is loaded and ready.

2. **No Brims Policy**:
   - By default, do not print with a brim. Slicer settings must have `brim_type` set to `"no_brim"`, `brim_width` set to `0`, and `skirt_loops` set to `0`.

3. **Adhesion & Speed Settings**:
   - First layer speeds: 15 mm/s for walls/perimeters, 20 mm/s for infill.
   - Layer height: 0.20 mm (first layer and subsequent layers).
   - Infill: 35% zig-zag (as set in each `process.json`), 4 wall loops.

4. **Startup G-Code Safety**:
   - Emitted G-code must use positive-Z purge travel (lift to Z=1 before lateral movement) to prevent nozzle scraping.
   - Use verified gentler cutter startup macros (press at 5 mm/s to 20 mm/s) to avoid frame impacts.

5. **Do Not Rebuild the Overview Automatically**:
   - Never rebuild `main_overview.FCStd` (`overview/build_overview.py`) after a CAD or parameter change, even when the changed part appears in it.
   - The user will say when to rebuild it. Until then, leave it as is and, if useful, mention that it is out of date.
