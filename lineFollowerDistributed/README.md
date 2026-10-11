# Distributed line follower

Desktop decides. The Pi relays. The Teensy samples the sensors and drives the motors. This is the hardware in `docs/line_folower.tex`. A run uses Wi-Fi only: no Ethernet cable on the Pi.

## Hardware

The robot is a Raspberry Pi Model B Plus with a Teensy 4.1 on the upper deck.

- **Control link.** Pi GPIO14/TX (header pin 8) to Teensy pin 0/RX1, and Teensy pin 1/TX1 to Pi GPIO15/RX (header pin 10). The Pi opens `/dev/serial0` at 115200 baud, 8N1. This is 3.3 V UART, not USB serial. Figure 3 in the TeX (`fig:pi-teensy-tb6612-wiring`) is this wiring.
- **Programming USB.** The Teensy USB plug stays in the Pi and appears as `/dev/ttyACM0` (`16c0:0483`). The document records firmware installed through that Pi USB port. The Mac does not see the Teensy. The Teensy USB power bridge is cut; the board is powered from the 5 V logic rail.
- **Sensors.** Modules A, B and C (left, center, right, looking forward) go to Teensy GPIO 25/26/27, which are A11/A12/A13. That is Figure 4 (`fig:pi-teensy-tcrt5000-wiring`).
- **Motors.** TB6612FNG. Left wheel is channel A (Teensy PWM 3, direction 4 and 5). Right wheel is channel B (PWM 9, direction 7 and 8). Standby is pin 6. `STOPALL` puts both bridges in standby. Polarity in the controller config is `rr`.
- **Timing wire.** Teensy pin 20 through 5 kΩ to Pi GPIO23 (header pin 16). `runClocksCalib` uses this edge. It does not drive the wheels. The white LED is Teensy pin 21 through 5 kΩ; the green LED is Pi GPIO24 (header pin 18).
- **Power.** One 2S pack. S1 feeds the 5 V logic rail (Pi, Teensy Vin, sensor VCC). S2 feeds the 6 V motor rail. Opening S2 leaves the computers on and removes motor power.

## Network

Robot runs use path A only.

| Machine | Address | Interface |
|---|---|---|
| pi2 | `10.0.0.20` | Wi-Fi to ASUS AirBears 3 |
| Desktop | `10.0.0.2` | Ethernet into the ASUS |
| ASUS | `10.0.0.1` | transports packets, does not steer |

`runOperator` refuses any Pi address other than `10.0.0.20`. Paths B (`10.0.0.120`) and C (`192.168.68.220`) are for SSH maintenance when an Ethernet cable is plugged in. Unplug that cable before a robot run.

UDP is Pi `:5000` and Desktop `:5001`. The Pi controller takes those addresses from the SSH connection that started it. The desktop operator binds the address printed in that SSH route.

## Where the files are

| Path | In git | What it is |
|---|---|---|
| `lineFollowerDistributed/` | yes | C++ sources, Teensy sketch, both `config.*.json` files, this README |
| `lineFollowerDistributedBin/` | no | `run_operator` and `clock_probe` on the Mac; `controller`, `gpio_clock` and `clock_probe` on the Pi |
| `onDesktop/` | yes | the six commands below |
| `config/photo3_calib.json` | yes | latest sensor calibration, written on the Pi |
| `config/clock_calib.json` | yes | latest clock summary, built on the Mac and stored on the Pi |
| `config/boot.json` | yes | hash8 for the Pi boot of those calibrations |

The Mac binary and the Pi binary are different builds of different sources. `updateBin` compiles each one on the machine that runs it. Do not copy `run_operator` onto the Pi or `controller` onto the Mac.

`config.json` beside each binary is a copy made by `make`. The Pi copy comes from `config.controller.json`. The Mac copy comes from `config.operator.json`. The program reads that file from the directory of the executable, then applies command-line flags.

## Calibration

Sensor calibration is manual. Log into the Pi and run `photo3_calib.py` from `onPi/localControl/`. It writes `config/photo3_calib.json`. Do this with the wheels free to turn, and with no controller running, because the script owns `/dev/serial0`.

When the Pi controller starts, it opens that latest file and hashes it. `runOperator` copies the same file to the Mac and passes it to `run_operator`. Both sides must hash identical bytes before the session arms. There is no frozen hash in `config.controller.json`.

## Commands

All of these are run on the Mac, from `onDesktop/`. They SSH to `pi2` with the key already in `~/.ssh/config`. Path B is `pi2-asus-eth` and path C is `pi2-home-eth`. The login account is `pi`.

```bash
./makeBin              # compile both programs and flash the Teensy
./updateBin            # compile both programs, leave the firmware alone
./statusPlant          # report whether a run can start
./runOperator          # start the Pi controller, then the Mac operator
./runOperatorVideo     # record the webcam, run the plant, write telemetry.mp4
./runClocksCalib       # measure Mac-Pi delay and Pi-Teensy delay (path B)
./stopPlant            # interrupt a run or a clock calibration
```

`makeBin` and `updateBin` accept `--pi 10.0.0.20`. `runOperator` accepts:

- `--duration SECONDS` (default 30). The same value is given to both programs. It is the driving window after the job handshake, not the whole process lifetime.
- `--speed 0..1` (default 0.5 from the operator config).
- `--disable-motors`. Omit this and the wheels turn. With the flag, motors stay stopped and the Pi writes `STOPALL`. The choice is passed to both programs.

`statusPlant` exits 0 only when all of these are true: Wi-Fi answers, the Pi controller is not running, the Mac operator is not running, `/dev/serial0` exists, and `config/photo3_calib.json` has white and black means for A, B, and C. Those means are used as valid. It also prints whether `/dev/ttyACM0` is present. That USB port is required by `makeBin`, not by a motor-off run.

`runClocksCalib` measures clock delay. It needs path B: Pi Ethernet `10.0.0.120` in an ASUS LAN port, Pi Wi-Fi still at `10.0.0.20`, and Desktop Ethernet `10.0.0.2`. It runs the Mac–Pi UDP probe on both of those Pi addresses, then `gpio_clock` (200 edges, Teensy pin 20 to Pi GPIO23). It does not arm the wheels. At job start the Pi controller reads `boot.json`, `clock_calib.json`, and `photo3_calib.json` and sends those bytes to the operator in one UDP message. When the clock and boot hash8 values match, the reported transmit uncertainty is the Wi-Fi half-width plus the Pi–Teensy residual and drift since calibration. A different hash8 means the Pi was rebooted, so the run reports the raw delivery upper bound and uses that bound as the uncertainty.

`stopPlant` sends SIGINT to `run_operator` and to the Pi `controller`. It also stops `clock_probe` and `gpio_clock` if a calibration is in progress. The controller's exit path writes `STOPALL` and `STREAM 0` on the UART.

## What a run does

1. `runOperator` checks `statusPlant`.
2. It SSHes to the Pi and starts `start_from_ssh.sh`, which runs `controller` with `--bind` and `--peer` taken from that SSH connection.
3. The controller opens `/dev/serial0`, checks the firmware banner, and waits for the desktop job. Motors are stopped.
4. The Mac operator starts. The controller sends `boot.json`, `clock_calib.json`, and `photo3_calib.json` over UDP, then the operator arms the session. It compares the two hash8 values and reports transmit latency with the matching uncertainty. It does not measure the clocks again.
5. Sensor samples flow Teensy → Pi → Desktop. Desktop computes left/right PWM and the Pi applies those targets. With `--disable-motors`, the Pi does not apply them.
6. Duration expiry, Ctrl-C, or `stopPlant` stops both sides. The controller confirms Teensy standby on the way out.

`gpio_clock` is built next to `controller`. `runClocksCalib` runs it for the Pi–Teensy half. `clock_probe` is built on both machines for the Mac–Pi half.

`runOperatorVideo` uses `onDesktop/video.json` for the camera name and the recording budget. It starts the webcam, waits until the recorder prints `RECORDING`, then calls `runOperator`. The recording length is `startup_s + duration + tail_s` and must fit in `max_record_s`. The default duration is 8 seconds. Wheels turn unless `--disable-motors` is passed. One directory under `tmp/video/` holds the raw movie, both event logs, `overlay.json`, a preview PNG, and `telemetry.mp4`. The banner’s transmit row is rebuilt from `clock_calib.json` when the boot hash matches. Round trip and spare use the Teensy clock when the firmware reports motor receipt times.

## Firmware

The sketch is `firmware/halo_teensy_firmware/halo_teensy_firmware.ino`. `makeBin` compiles it on the Mac with the Teensy Arduino package at:

`/Users/balewski/shared_volumes/2026_robot1/robot0/teensy41/tools/arduino-cli`

It then copies the hex to the Pi and runs `teensy_loader_cli --mcu=TEENSY41 -s -w`. The loader and its `libusb-0.1` library are copied into `lineFollowerDistributedBin/` and `lineFollowerDistributedBin/loader-libs/` from `/home/pi/old-robot0/teensy41/tools` if they are not already there. `makeBin` aborts before compiling when `/dev/ttyACM0` or `/dev/serial0` is missing.

`updateBin` is the ordinary compile. Use it when the firmware should stay as it is.

## Logs

The Pi controller log for a launch is `/tmp/controller.log`, and its pid is `/tmp/controller.pid`. Both are on the Pi RAM disk. The controller writes `events.csv` under `/tmp/` as well. `summary.json` stays in `lineFollowerDistributedBin/out/` on the machine where that program runs. The Mac operator record is under the repository `tmp/` directory. Those directories are gitignored.
