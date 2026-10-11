# Distributed line follower

Desktop decides. The Pi relays. The Teensy samples the sensors and drives the motors. This is the hardware in `docs/line_folower.tex`. A run uses Wi-Fi only: no Ethernet cable on the Pi.

## Hardware

The robot is a Raspberry Pi Model B Plus with a Teensy 4.1 on the upper deck.

- **Control link.** Pi GPIO14/TX (header pin 8) to Teensy pin 0/RX1, and Teensy pin 1/TX1 to Pi GPIO15/RX (header pin 10). The Pi opens `/dev/serial0` at 115200 baud, 8N1. This is 3.3 V UART, not USB serial. Figure 3 in the TeX (`fig:pi-teensy-tb6612-wiring`) is this wiring.
- **Programming USB.** The Teensy USB plug stays in the Pi and appears as `/dev/ttyACM0` (`16c0:0483`). The document records firmware installed through that Pi USB port. The Mac does not see the Teensy. The Teensy USB power bridge is cut; the board is powered from the 5 V logic rail.
- **Sensors.** Modules A, B and C (left, center, right, looking forward) go to Teensy GPIO 25/26/27, which are A11/A12/A13. That is Figure 4 (`fig:pi-teensy-tcrt5000-wiring`).
- **Motors.** TB6612FNG. Left wheel is channel A (Teensy PWM 3, direction 4 and 5). Right wheel is channel B (PWM 9, direction 7 and 8). Standby is pin 6. `STOPALL` puts both bridges in standby. Polarity in the controller config is `rr`.
- **Timing wire.** Teensy pin 20 through 5 kΩ to Pi GPIO23 (header pin 16). The startup clock check uses this edge. It does not drive the wheels. The white LED is Teensy pin 21 through 5 kΩ; the green LED is Pi GPIO24 (header pin 18).
- **Power.** One 2S pack. S1 feeds the 5 V logic rail (Pi, Teensy Vin, sensor VCC). S2 feeds the 6 V motor rail. Opening S2 leaves the computers on and removes motor power.

## Network

Robot runs use path A only.

| Machine | Address | Interface |
|---|---|---|
| Pi | `10.0.0.149` | Wi-Fi to ASUS AirBears 3 |
| Desktop | `10.0.0.2` | Ethernet into the ASUS |
| ASUS | `10.0.0.1` | transports packets, does not steer |

`runOperator` refuses any Pi address other than `10.0.0.149`. Paths B (`10.0.0.24`) and C (`192.168.68.125`) are for SSH maintenance when an Ethernet cable is plugged in. Unplug that cable before a robot run.

UDP is Pi `:5000` and Desktop `:5001`. The Pi controller takes those addresses from the SSH connection that started it. The desktop operator binds the address printed in that SSH route.

## Where the files are

| Path | In git | What it is |
|---|---|---|
| `lineFollowerDistributed/` | yes | C++ sources, Teensy sketch, both `config.*.json` files, this README |
| `lineFollowerDistributedBin/` | no | `run_operator` on the Mac; `controller` and `gpio_clock` on the Pi |
| `onDesktop/` | yes | the five commands below |
| `onPi/localControl/out/photo3_calib.latest.json` | no | latest sensor calibration, written on the Pi |

The Mac binary and the Pi binary are different builds of different sources. `updateBin` compiles each one on the machine that runs it. Do not copy `run_operator` onto the Pi or `controller` onto the Mac.

`config.json` beside each binary is a copy made by `make`. The Pi copy comes from `config.controller.json`. The Mac copy comes from `config.operator.json`. The program reads that file from the directory of the executable, then applies command-line flags.

## Calibration

Sensor calibration is manual. Log into the Pi and run `photo3_calib.py` from `onPi/localControl/`. It writes `out/photo3_calib.latest.json` next to itself. Do this with the wheels free to turn, and with no controller running, because the script owns `/dev/serial0`.

When the Pi controller starts, it opens that latest file and hashes it. `runOperator` copies the same file to the Mac and passes it to `run_operator`. Both sides must hash identical bytes before the session arms. There is no frozen hash in `config.controller.json`.

## Commands

All of these are run on the Mac, from `onDesktop/`. They SSH to `pi@10.0.0.149` with the key already in `~/.ssh/config`.

```bash
./makeBin              # compile both programs and flash the Teensy
./updateBin            # compile both programs, leave the firmware alone
./statusPlant          # report whether a run can start
./runOperator          # start the Pi controller, then the Mac operator
./stopPlant            # interrupt both if a run is stuck
```

`makeBin` and `updateBin` accept `--pi 10.0.0.149`. `runOperator` accepts:

- `--duration SECONDS` (default 30). The same value is given to both programs. It is the driving window after the startup clock handshake, not the whole process lifetime.
- `--speed 0..1` (default 0.5 from the operator config).
- `--disable-motors`. Omit this and the wheels turn. With the flag, motors stay stopped and the Pi writes `STOPALL`. The choice is passed to both programs.
- `--timingTeensyPi`. Omit this and the Pi–Teensy GPIO clock measurement is skipped. With the flag, both programs run the three flashes and 200 timing pulses before the UDP probes. That measurement does not steer the wheels.

`statusPlant` exits 0 only when all of these are true: Wi-Fi answers, the Pi controller is not running, the Mac operator is not running, `/dev/serial0` exists, and the calibration file is `complete` and `valid`. It also prints whether `/dev/ttyACM0` is present. That USB port is required by `makeBin`, not by a motor-off run.

`stopPlant` sends SIGINT to `run_operator` and to the Pi `controller`. The controller's exit path writes `STOPALL` and `STREAM 0` on the UART.

## What a run does

1. `runOperator` checks `statusPlant`, then copies the calibration.
2. It SSHes to the Pi and starts `start_from_ssh.sh`, which runs `controller` with `--bind` and `--peer` taken from that SSH connection.
3. The controller opens `/dev/serial0`, checks the firmware banner, and waits for the desktop job. Motors are stopped.
4. The Mac operator starts. With `--timingTeensyPi`, startup runs the Pi–Teensy GPIO clock handshake (LED flashes, timing wire, no wheel motion). Either way it then runs the UDP time probes and arms the session.
5. Sensor samples flow Teensy → Pi → Desktop. Desktop computes left/right PWM and the Pi applies those targets. With `--disable-motors`, the Pi does not apply them.
6. Duration expiry, Ctrl-C, or `stopPlant` stops both sides. The controller confirms Teensy standby on the way out.

`gpio_clock` is built next to `controller` because the controller forks it for that startup handshake. It is not a separate user command. Video capture and the old unit-test programs were not ported.

## Firmware

The sketch is `firmware/halo_teensy_firmware/halo_teensy_firmware.ino`. `makeBin` compiles it on the Mac with the Teensy Arduino package at:

`/Users/balewski/shared_volumes/2026_robot1/robot0/teensy41/tools/arduino-cli`

It then copies the hex to the Pi and runs `teensy_loader_cli --mcu=TEENSY41 -s -w`. The loader and its `libusb-0.1` library are copied into `lineFollowerDistributedBin/` and `lineFollowerDistributedBin/loader-libs/` from `/home/pi/old-robot0/teensy41/tools` if they are not already there. `makeBin` aborts before compiling when `/dev/ttyACM0` or `/dev/serial0` is missing.

`updateBin` is the ordinary compile. Use it when the firmware should stay as it is.

## Logs

The Pi controller log for a launch is `lineFollowerDistributedBin/controller.log`, and its pid is `controller.pid` in that same directory. Both programs also write a timestamped report under `lineFollowerDistributedBin/out/` on the machine where they run. Those directories are gitignored.
