# ESP8266 Emulator

A source-based ESP8266/LX106 emulator with a local web interface for inspecting
firmware execution, UART output and GPIO. Load your own flash image, drive inputs,
observe pin transitions and capture repeatable execution reports.

The emulator combines an Xtensa LX106 interpreter, C implementations of ROM
services and peripheral register models. It targets CALL0 firmware and E9-format
flash images, including Arduino/NONOS SDK boot flows. Compatibility is incomplete;
an unfamiliar image may need additional ROM or peripheral support. Firmware, SDK
libraries and mask-ROM dumps are not included.

## Quick start

Use Linux or WSL Ubuntu with **GCC, GNU Make and Python 3**. The interpreter uses
GNU C11 extensions; the GUI and Python tools use the standard library. No Python
packages or frontend build are required.

```sh
git clone https://github.com/digitake/esp8266-emulator.git
cd esp8266-emulator
make
make check
```

`make check` runs CPU/peripheral sanitizer tests, Python tests and a synthetic
watchdog test. It needs no external firmware or network service.

Start the GUI from Linux or WSL:

```sh
bash run-gui.sh
```

Open **http://127.0.0.1:8266/** and drop in a `firmware.bin` intended for flash
address zero. Include its matching `firmware.elf` for symbol-based Arduino
`setup()` and `loop()` counts. To select an image at startup:

```sh
bash run-gui.sh --bin /path/to/firmware.bin --elf /path/to/firmware.elf
```

From Windows PowerShell:

```powershell
.\run-gui.ps1 -Bin C:\path\to\firmware.bin -Elf C:\path\to\firmware.elf
```

The Windows launcher builds and runs inside WSL Ubuntu and opens the browser.
Use `-Distro <name>` to select another distribution, or `-Port 8276` if the
default port is occupied. Ctrl+C stops the server.

## GPIO and firmware inspection

The default view exposes all 17 GPIOs without assuming a board layout. Drive
inputs LOW/HIGH or release them; inspect modes, levels and edge counts. An
optional [GPIO profile](examples/gpio-profile.json) assigns your own button,
relay and LED roles for a faceplate:

```sh
bash run-gui.sh --profile-file /path/to/gpio-profile.json
```

Use `-ProfileFile C:\path\to\gpio-profile.json` with the Windows launcher.
A profile changes the display and input mapping; application behavior remains
in the firmware.

The GUI also provides live UART, boot stages, reset cause, reset/power/erase
controls and speed selection. Known credential fields are masked before GUI
output. Firmware configuration persists in a flash file per uploaded image.

The server binds **127.0.0.1 only**, validates Host and Origin, and caps uploads
at 16 MiB. Images are stored by SHA-256 under
`~/.cache/esp8266-emulator/images/`. Use `--workdir /path/to/cache` with the Linux
launcher for an isolated session.

## Headless use

```sh
build/esp8266emu --flash /path/to/firmware.bin \
  --speed 0 --max-seconds 10 --no-stdin --quiet
```

Select physical chip and flash settings with `--chip esp8266`, `esp12e`,
`esp12f`, `esp8285` (1 MiB) or `esp8285h16` (2 MiB). External-flash profiles
default to 4 MiB and accept `--flash-size BYTES`; embedded-flash profiles have
fixed capacity. `--flash-id HEX` selects the flash chip's 24-bit JEDEC ID.
Without `--chip`, the emulator uses ESP8266 identity and the image header's
flash capacity. An image larger than the selected capacity is rejected.
These profiles set identity and capacity; they do not establish support for
every peripheral or firmware built for that chip.

Capture a masked report with input hashes, execution counts and pin edges:

```sh
python3 -I scripts/evidence.py \
  --bin /path/to/firmware.bin --elf /path/to/firmware.elf \
  --seconds 10 --out /tmp/boot.json
```

For repeatable firmware checks, supply your own images and references through a
[regression manifest](examples/regression-manifest.json). See
[testing and evidence](docs/TESTING.md) for the workflow.

## Scope and limitations

The implementation includes GPIO/UART/timers, NOR flash operations, interrupt
and exception handling, a task scheduler and a hardware watchdog. Minimal
no-ACK MAC completion and a MAC timer support selected SDK paths. These models
do not provide a radio.

The default build is offline. An optional [virtual station](docs/NETWORKING.md)
connects image-bound Arduino 3.1.2/lwIP2 firmware to a host NAT backend. This
experimental adapter needs matching BIN/ELF profiles; compatibility with other
SDKs, SoftAP and physical radio behavior is not established. The separate OTA
rehearsal tool injects staged bytes and a boot command to test eboot copying.

Timing assumes one cycle per instruction and instantaneous flash operations.
This is a functional debugging tool, not a cycle-accurate or electrical board
model. Physical-board timing has not been validated.

## Project guide

- [Current scope](docs/STATUS.md) and [architecture/event protocol](docs/ARCHITECTURE.md)
- [Testing, regression fixtures and performance](docs/TESTING.md)
- [Network feasibility](docs/NETWORKING.md) and [physical-board comparison](docs/BOARD-COMPARISON.md)
- [Contributing](CONTRIBUTING.md) and [reference notices](NOTICE.md)

Source lives in `src/`, the local interface in `gui/`, command-line tools in
`scripts/`, synthetic tests in `tests/`, and reusable input examples in `examples/`.

## License

[MIT](LICENSE). User-supplied firmware, SDKs and toolchains retain their own
licenses; this project's license does not relicense those inputs.
