# Agent Instructions: 3D Printing on Local 3D Printer

This unified document guides automated agents and developers on preparing, slicing, validating, and sending 3D print jobs to the user's local 3D printer for all components in `assembly_sep24`.

---

## 1. Printer & Network Setup

- **Printer Model**: QIDI Tech Q2 (CoreXY, Klipper firmware with Moonraker web API)
- **Moonraker API Host**: `http://192.168.68.123:7125`
- **Build Volume**: 270 × 270 × 260 mm
- **Default Material**: PETG (Black)
- **Standard Temperatures**: Bed 80 °C, Hotend 245–250 °C (0.4 mm hardened nozzle)
- **Slicer Path (macOS)**: `/Applications/QIDIStudio.app/Contents/MacOS/QIDIStudio`
- **FreeCAD Python Runtime**: `/Applications/FreeCAD.app/Contents/Resources/bin/python` with `PYTHONPATH=/Applications/FreeCAD.app/Contents/Resources/lib`

---

## 2. Mandatory Agent Rules & Safety Policies

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

---

## 3. End-to-End Printing Workflow

### Step 1: Export Manufacturing Meshes from FreeCAD
Run the corresponding `export_print.py` script using FreeCAD's Python interpreter:
```bash
PYTHONPATH=/Applications/FreeCAD.app/Contents/Resources/lib \
/Applications/FreeCAD.app/Contents/Resources/bin/python \
cad_parts/assembly_sep24/<print_dir>/export_print.py
```
This inspects the native `.FCStd` source, checks solid validity, harmonizes normals, closes non-manifold facets, and outputs:
- Closed `.stl` files for each component.
- Packed `.3mf` layout file positioned on the 270 × 270 mm bed.
- `export_validation.json` recording source and mesh SHA-256 hashes.

All of these are written to `cad_parts/assembly_sep24/tmp/`, a git-ignored working area for regenerable files; overwrite freely.

### Step 2: Slice with QIDIStudio CLI
Execute QIDIStudio headlessly using the local configuration JSONs:
```bash
cd cad_parts/assembly_sep24/<print_dir>
mkdir -p ../tmp/<slice_dir>
/Applications/QIDIStudio.app/Contents/MacOS/QIDIStudio \
  --load-settings 'machine.json;process.json' \
  --load-filaments filamebly_sep24/<print_dir>
mkdir -p ../tmp/<slice_dir>
/Applications/QIDIStudio.app/Contents/MacOS/QIDIStudio \
  --load-settings 'machine.json;process.json' \
  --load-filaments filament.json \
  --load-defaultfila \
  --arrange 0 --orient 0 --slice 0 \
  --export-3mf <output_name>.3mf \
  --outputdir "$(cd ../tmp/<slice_dir> && pwd)" \
  ../tmp/<layout_name>.3mf
```
`plate_1.gcode` is written next to the sliced `.3mf` in `tmp/<slice_dir>/`; use it directly.

### Step 3: Check Printer Readiness
Verify via Moonraker API that the printer is connected, idle, and ready:
```bash
curl -s http://192.168.68.123:7125/printer/info | jq .
curl -s http://192.168.68.123:7125/printer/objects/query\?print_stats\&heater_bed\&extruder | jq .
```
Ensure `print_stats.state` is `"standby"` or `"ready"`. Do not upload or start if another print is in progress.

### Step 4: Generate As-Printed Diagram & Await User Confirmation
1. Parse the sliced `plate_1.gcode` or extract metadata to produce the as-printed bed layout PNG (`bed_layout_with_supports.png`).
2. Display the generated PNG directly on screen using:
   ```bash
   open <output_dir>/bed_layout_with_supports.png
   ```
3. Summarize to the user:
   - Components included on the plate.
   - Orientation (which face is against the bed at Z=0, direction of layer bonds).
   - Support structures: exact location and purpose of support pillars (if any).
   - Estimated print duration and material usage in grams.
   - Request explicit confirmation for: layout approved, bed empty and clean, and PETG loaded.

### Step 5: Upload G-code to Printer
Once approved, upload the sliced G-code file to Moonraker:
```bash
curl -X POST -F "file=@<job_name>.gcode" \
  http://192.168.68.123:7125/server/files/upload
```

### Step 6: Start Print Job
Send the command to begin printing:
```bash
curl -X POST -H "Content-Type: application/json" \
  -d '{"filename": "<job_name>.gcode"}' \
  http://192.168.68.123:7125/printer/print/start
```

### Step 7: Emergency Controls & Monitoring
- **Query live progress**:
  ```bash
  curl -s http://192.168.68.123:7125/printer/objects/query\?print_stats\&virtual_sdcard | jq .
  ```
- **Pause print**:
  ```bash
  curl -X POST http://192.168.68.123:7125/printer/print/pause
  ```
- **Cancel / Abort print**:
  ```bash
  curl -X POST http://192.168.68.123:7125/printer/print/cancel
  ```

---

## 4. Component Print Directory Map

| Print Directory | Source CAD File | Components Included | Orientation & Notes |
| :--- | :--- | :--- | :--- |
| `print_lower_deck` | `main_lower_deck.FCStd` | `ChassisWithLeftMotorA`, `LeftMotorClamp`, `CableClamp1–3` | Deck top face down on bed; motor clamp flat with screw recesses down. |
| `print_pillars_sensor` | `main_pillars.FCStd`, `main_photo_sensor.FCStd` | `FrontPillarFrame`, `AftPillarFrame`, `SensorHolder` | Front rotated +90° on braced face; aft -90°; sensor holder 180° with removable supports. |
| `print_upper_deck` | `main_upper_deck.FCStd` | `TeensyPlatform`, `BoardRetainingClamp`, `AmmeterSupportC` | Platform underside flat on bed; clamp and ammeter support flat beside deck. |
| `print_roof_conical` | `main_roof.FCStd` | `MainRoof` | Inverted (180° around X): broad roof face on bed, columns upward. |
| `print_photo_sensor` | `main_photo_sensor.FCStd` | `SensorHolder`, `CoverStrip`, `BuzzerHolder` | Holder inverted (180° about X), main face on bed, supports only in the three sensor vaults; cover strip broad face down; buzzer holder ring down. Layer height 0.20 mm (0.10 mm feeds filament too slowly and jammed the extruder in a warm chamber). |
