# ESP8266 Emulator

[![Build and test](https://github.com/digitake/esp8266-emulator/actions/workflows/ci.yml/badge.svg)](https://github.com/digitake/esp8266-emulator/actions/workflows/ci.yml)

Run ESP8266 firmware on your PC without a board. The emulator boots an unmodified
`firmware.bin`, shows its serial output, and lets you press its buttons and watch
its pins change. It can also record repeatable test reports.

![The emulator GUI running examples/hello](docs/images/gui.png)

*The GUI running [examples/hello](examples/hello): a live drawing of the module
with every pin, a faceplate for the button, relay and LED, and the serial console.*

## Is it for you?

Use it to:

- find out why firmware crashes or keeps resetting, without flashing a board;
- test buttons, relays and LEDs with exact, repeatable timing;
- check that firmware still behaves the same after a change;
- rehearse Arduino OTA updates.

Know its limits:

- **ESP8266 only**, not ESP32. Developed around Arduino core 3.1.2 and the
  NONOS SDK. Other firmware may call ROM functions that are not implemented yet.
- **No radio.** The GUI runs offline, so Wi-Fi scans find nothing. An
  experimental [virtual network](docs/NETWORKING.md) for headless runs can connect
  Arduino 3.1.2 firmware to your host.
- **Idealized timing.** It counts one instruction per cycle and treats flash
  writes as instant. It is a functional debugger, not a cycle-accurate or
  electrical model.
- **Bring your own firmware.** No firmware, SDK or ROM dump is included. A small
  example sketch is.

## Quick start

### 1. Install the tools

You need Linux, or Windows 10/11 with WSL running Ubuntu, plus GCC, Make and
Python 3. On Ubuntu:

```sh
sudo apt update
sudo apt install -y build-essential python3 git
```

On Windows, first run `wsl --install -d Ubuntu` in an administrator PowerShell,
then open the Ubuntu app and run the commands above. No Python packages, Node.js
or frontend build are needed.

### 2. Build and self-test

```sh
git clone https://github.com/digitake/esp8266-emulator.git
cd esp8266-emulator
make
make check
```

`make check` uses no firmware. It ends with
`watchdog busy-loop evidence: PASS`.

### 3. Get a firmware image

The emulator boots the complete image that would be written at flash address
0x0. PlatformIO writes it to `.pio/build/<env>/firmware.bin`. Keep the matching
`firmware.elf` from the same build: with it the GUI shows when `setup()` and
`loop()` start.

If you have no firmware yet, build the example with
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html):

```sh
pio run -d examples/hello
```

The example prints to the serial port, blinks GPIO2, switches GPIO5 while GPIO0
is held LOW, and echoes what you type.

### 4. Start the GUI

From Linux or the Ubuntu shell:

```sh
bash run-gui.sh --bin examples/hello/.pio/build/esp12e/firmware.bin \
  --elf examples/hello/.pio/build/esp12e/firmware.elf \
  --profile-file examples/gpio-profile.json
```

Then open **http://127.0.0.1:8266/**. From Windows PowerShell, in the cloned
folder, use this instead. It builds and runs inside WSL and opens the browser:

```powershell
.\run-gui.ps1 -Bin examples\hello\.pio\build\esp12e\firmware.bin `
  -Elf examples\hello\.pio\build\esp12e\firmware.elf `
  -ProfileFile examples\gpio-profile.json
```

Things to try:

- Click the **GPIO0** pad. The console prints `button pressed`, and GPIO5 goes HIGH.
- Type `hi` into the box under the console. The firmware answers `you typed: hi`.
- Press **Reset**, then **Power cycle**, and watch the Boot card's reset history.

Run `bash run-gui.sh` without `--bin` and drag any `.bin` and `.elf` onto the
page instead. Press Ctrl+C in the terminal to stop it.

## What the GUI shows

| Part | What it gives you |
| --- | --- |
| Header | Image name and hash, emulated time, speed (1×, ¼× or max), MIPS, boot count, current stage |
| Board | ESP-12F drawing with each pin's live level and mode. Click a pad to drive it LOW, HIGH or release it. |
| Boot | ROM → eboot → app → SDK → `setup()` → `loop()` timeline, and the reason for every reset |
| Signals | Logic-analyzer trace of recent pin changes |
| Network | Soft AP and station state, as reported by the firmware |
| Serial console | UART output with credentials masked; send text to the firmware |

The **Reset**, **Power cycle** and **Erase flash** buttons act like the real
thing. Flash contents the firmware saves, such as settings, survive restarts.
See [Use the GUI](docs/USER-MANUAL.md#6-use-the-gui) for every control.

## Beyond the GUI

| I want to… | Start with | Read more |
| --- | --- | --- |
| Run in a terminal or CI | `build/esp8266emu --flash fw.bin --speed 0 --max-seconds 10 --no-stdin --quiet` | [Run headless](docs/USER-MANUAL.md#9-run-headless) |
| Press buttons at exact times | A scenario file: `1000 in 0 0` drives GPIO0 LOW at 1 s | [Scenario scripts](docs/USER-MANUAL.md#10-scenario-scripts) |
| Find why a boot fails | The Boot card, the **emulator log** toggle, `--strict` | [Debug a boot](docs/USER-MANUAL.md#11-debug-firmware-that-does-not-boot) |
| Prove nothing changed | `scripts/evidence.py`, then `scripts/regression.py` | [Reports and regression](docs/USER-MANUAL.md#12-reports-regression-and-ota-tests) |
| Test an OTA update | `scripts/ota_rehearsal.py` with your own images, offline | [OTA tests](docs/USER-MANUAL.md#12-reports-regression-and-ota-tests) |
| Give firmware a network | `make network`, then `build/esp8266emu-net --net-profile …` | [Virtual network](docs/USER-MANUAL.md#virtual-network-experimental) |
| Emulate an ESP8285 or a given flash size | `--chip esp8285` or `--flash-size 0x200000` | [Chip and flash](docs/USER-MANUAL.md#chip-and-flash-options) |

## How it works

```mermaid
flowchart LR
    image["firmware.bin + .elf"] --> emu
    subgraph emu ["build/esp8266emu"]
        cpu["LX106 CPU interpreter"] --> rom["ROM services written in C"]
        cpu --> io["GPIO, UART, timers, flash, watchdog"]
    end
    emu -->|"JSON events (stdout)"| server["gui/server.py"]
    server -->|"commands (stdin)"| emu
    server -->|"live events, 127.0.0.1 only"| page["browser: gui/index.html"]
    page -->|"clicks and uploads"| server
```

The firmware runs instruction by instruction, exactly as it was built. The
emulator stands in for the chip's mask ROM and its peripherals; it never patches
the image. The GUI server only listens on 127.0.0.1 and refuses other host names.
[Architecture](docs/ARCHITECTURE.md) has the details.

## Repository layout

| Folder | Contents |
| --- | --- |
| `src/` | The emulator in C: CPU, ROM services, peripherals, networking adapter |
| `gui/` | Local web server (Python standard library) and the single-page GUI |
| `scripts/` | Reports, regression, OTA, profiling and symbol tools |
| `tests/` | C and Python tests run by `make check` |
| `examples/` | The hello sketch, a GPIO profile and a regression manifest |
| `scenarios/` | A sample timed-input script |
| `docs/` | Manual, scope, architecture, testing and networking notes |

## Documentation

1. [User manual](docs/USER-MANUAL.md): start here. Install, GUI, headless runs,
   debugging, troubleshooting and a glossary.
2. [Current scope](docs/STATUS.md): what is implemented and what is not.
3. [Testing](docs/TESTING.md): regression fixtures, OTA tests and performance.
4. [Virtual networking](docs/NETWORKING.md): the experimental network adapter.
5. [Architecture](docs/ARCHITECTURE.md): how the emulator works inside, and its event protocol.
6. [Board comparison](docs/BOARD-COMPARISON.md): comparing against a physical board.

## Contributing

Bug reports and fixes are welcome. Run `make check` before you send a change,
and see [CONTRIBUTING.md](CONTRIBUTING.md) for what a good report includes.
Keep firmware, flash files and credentials out of issues and commits.

## License

[MIT](LICENSE). Firmware, SDKs and toolchains you use with the emulator keep
their own licenses. See [NOTICE.md](NOTICE.md) for reference material.
