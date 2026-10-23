# User manual

This manual explains how to install the ESP8266 Emulator, load firmware and work
with the web GUI, the headless runner and the report tools. It is written for
people who want to run, inspect or test ESP8266 firmware on a PC. For internals
see [architecture](ARCHITECTURE.md); for the exact rules of firmware regression
see [testing](TESTING.md).

**New here?** Read sections 1 and 2, then run the bundled example as described in
[section 2](#2-quick-start). The [glossary](#16-glossary) explains terms such as
eboot, ELF and RTC memory.

## Contents

1. [What the emulator does](#1-what-the-emulator-does)
2. [Quick start](#2-quick-start)
3. [Install and build](#3-install-and-build)
4. [Prepare your firmware](#4-prepare-your-firmware)
5. [Start the GUI](#5-start-the-gui)
6. [Use the GUI](#6-use-the-gui)
7. [Resets, flash and saved state](#7-resets-flash-and-saved-state)
8. [GPIO profiles](#8-gpio-profiles)
9. [Run headless](#9-run-headless)
10. [Scenario scripts](#10-scenario-scripts)
11. [Debug firmware that does not boot](#11-debug-firmware-that-does-not-boot)
12. [Reports, regression and OTA tests](#12-reports-regression-and-ota-tests)
13. [Privacy and local files](#13-privacy-and-local-files)
14. [Troubleshooting](#14-troubleshooting)
15. [Command summary](#15-command-summary)
16. [Glossary](#16-glossary)

## 1. What the emulator does

The emulator runs an unmodified ESP8266 flash image on your computer. An LX106 CPU
interpreter executes the firmware, C code stands in for the chip's mask ROM, and
register-level models cover GPIO, UART, timers, flash and the hardware watchdog.
With it you can:

- watch the firmware boot and read its serial output;
- drive GPIO inputs such as buttons, and see outputs such as relays and LEDs change;
- reset, power-cycle or erase the emulated chip;
- type into the serial port;
- record repeatable execution reports and compare them exactly;
- rehearse Arduino OTA updates, offline or over a virtual network.

Know the limits before you start:

- **No radio.** Firmware can start a soft AP or scan, but nothing can join it and
  scans find no access points. Instead, an experimental
  [virtual network](#virtual-network-experimental) gives Arduino core 3.1.2
  firmware a Wi-Fi station connection. Once libslirp is installed, the GUI turns it
  on by itself for a `.bin` dropped with its `.elf`. By default the firmware
  reaches only your own computer.
- **Idealized timing.** One instruction per cycle, ideal clocks and instant flash
  writes. It is a functional debugging tool, not a cycle-accurate or electrical
  board model.
- **Partial compatibility.** ROM and SDK support was developed around Arduino core
  3.1.2 and the NONOS SDK. Other firmware may call something that is not
  implemented; [section 11](#11-debug-firmware-that-does-not-boot) shows how to
  find out.

No firmware, SDK library or ROM dump is included. You supply the image, or build
the small example sketch in [examples/hello](../examples/hello).

## 2. Quick start

Install the tools first ([section 3](#3-install-and-build)), then get the code:

```sh
git clone https://github.com/digitake/esp8266-emulator.git
```

**Start it with one command.** On Windows, double-click `emulator.cmd` in the
cloned folder, or run it from a terminal. It builds the emulator inside WSL, starts
the GUI and opens **http://127.0.0.1:8266/** in your browser. On Linux, or in the
Ubuntu shell, run `bash run-gui.sh` in the folder and open that address yourself.

**Drop your firmware on the page.** Drag `firmware.bin` and `firmware.elf` from
your build folder onto the page. The chip boots at once. If libslirp is installed,
an Arduino core 3.1.2 image dropped with its `.elf` also gets Wi-Fi, which reaches
only your own computer ([Network](#network)).

To stop the emulator, press Ctrl+C in its window or close the window.

To start it from any folder by typing `esp8266emu`, run this once in the cloned
folder:

```powershell
.\emulator.cmd -Install
```

### Your first run with the example

If you have no firmware at hand, build [examples/hello](../examples/hello) with
[PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html).
The project pins PlatformIO's `espressif8266@4.2.1` platform, which ships Arduino
core 3.1.2. PlatformIO downloads the toolchain on the first build.

```sh
pio run -d examples/hello
bash run-gui.sh --bin examples/hello/.pio/build/esp12e/firmware.bin \
  --elf examples/hello/.pio/build/esp12e/firmware.elf \
  --profile-file examples/gpio-profile.json
```

If PlatformIO is installed on Windows rather than in WSL, run `pio run` from
PowerShell and start the GUI with `.\emulator.cmd -ProfileFile examples\gpio-profile.json`,
then drop the two files from `examples\hello\.pio\build\esp12e` on the page. Both
sides see the same folder.

What you should see:

1. The Boot card steps through ROM, eboot, app, SDK, `setup()` and `loop()` within
   about 0.13 emulated seconds.
2. The console prints `hello from the emulator`, then `uptime 500 ms` twice a
   second.
3. The GPIO2 pad and the faceplate's LED ring blink. GPIO2 also appears in the
   Signals card.
4. Click the GPIO0 pad, which drives it LOW. The console prints `button pressed`
   and GPIO5, the relay, turns HIGH. Click it twice more to release it.
5. Type `hi` in *Send text to UART0 RX* and press Enter. The firmware answers
   `you typed: hi`.

The rest of this manual explains each of these steps in detail.

## 3. Install and build

### Requirements

| Item | Requirement |
| --- | --- |
| Operating system | Linux, or Windows 10/11 with WSL running Ubuntu |
| Tools | GCC (GNU C11), GNU Make, Python 3.8 or later |
| Browser | Any current browser on the same computer |
| Optional, virtual network | `libslirp-dev`, `libglib2.0-dev` and `pkg-config`. With them, the GUI gives Arduino 3.1.2 images Wi-Fi, and `make network` builds the headless network runner. |
| Optional, example sketch | [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), only to build [examples/hello](../examples/hello) |

No Python packages, Node.js or frontend build are needed. On Ubuntu, native or
under WSL:

```sh
sudo apt update
sudo apt install -y build-essential python3 git
```

For the virtual network, also install:

```sh
sudo apt install -y libslirp-dev libglib2.0-dev pkg-config
```

On Windows, install WSL with Ubuntu first from an administrator PowerShell, then
open the Ubuntu app, create your Linux user and run the `apt` commands above:

```powershell
wsl --install -d Ubuntu
```

You can keep the repository on the Windows side, for example under
`C:\Users\you\Dev`. Ubuntu reaches it as `/mnt/c/Users/you/Dev/...`, and the
Windows launcher translates paths for you.

### Build and self-check

From the repository root (on Windows, in the Ubuntu shell):

```sh
make
make check
```

`make` builds `build/esp8266emu`. `make check` runs the CPU and peripheral tests
under AddressSanitizer and UndefinedBehaviorSanitizer, the Python tests and a
synthetic watchdog test. It needs no firmware or network. A passing run ends like
this:

```text
python3 -I tests/test_runner.py
..
----------------------------------------------------------------------
Ran 2 tests in 0.189s

OK
python3 -I scripts/watchdog_check.py --out build/watchdog-check.json
watchdog busy-loop evidence: PASS build/watchdog-check.json
```

The GUI launchers and report tools run `make` themselves, so later source updates
are rebuilt automatically. `make clean` deletes `build/`.

## 4. Prepare your firmware

### The flash image

The emulator boots one file: the complete image that would be written at flash
address **0x0**. Its first byte must be `0xE9`, the ESP8266 image header.

| Build system | Flash image | Matching ELF |
| --- | --- | --- |
| PlatformIO | `.pio/build/<env>/firmware.bin` | `.pio/build/<env>/firmware.elf` |
| Arduino IDE | Sketch → Export Compiled Binary writes the `.bin` under the sketch folder | The `.ino.elf` from the same build output |

- Arduino images start with the eboot bootloader and boot through it, as on a
  board.
- If your build splits the bootloader and the application into files for different
  flash offsets, merge them into one file that starts at 0x0 first. esptool's
  `merge_bin` command can do this. Bootloaders other than Arduino's eboot are less
  tested.
- A full flash read-back from a board is also a valid image. It brings that
  board's saved configuration, possibly including Wi-Fi credentials, with it.
  To load it in the GUI as the image, name it `.bin`; named `.flash` and dropped
  with the application's `.bin`, it is a [saved flash](#boot-with-saved-settings)
  instead, which keeps the virtual network.
- The flash size comes from the image header. A file larger than that size is
  refused. The headless runner can set another size with `--chip` or
  `--flash-size` (see [chip and flash options](#chip-and-flash-options)).
- ESP32-family images also start with `0xE9`, but they are not supported.

### The ELF file (optional, recommended)

Supply the `.elf` from the **same build** as the `.bin`. The emulator reads only
its symbol table, to find the Arduino `setup()` and `loop()` functions. With it you
get the `setup()` and `loop()` boot stages, exact `loop()` counts, and the
`--until-loops` stop condition. Without it the firmware runs the same way, but the
last stage shown is **SDK main loop**.

The GUI does not check that the ELF matches the image. An ELF from another build
gives wrong or missing counts. To see the addresses it provides:

```sh
python3 -I scripts/elfsyms.py /path/to/firmware.elf
```

This prints a line such as `setup=0x40201a2c loop=0x40201a60`; the addresses
differ for every build.

### Keep firmware out of Git

`.gitignore` excludes `*.bin`, `*.elf`, `*.log` and a top-level `local/` folder.
`local/` is a convenient place for your images, scenarios and reference reports.

## 5. Start the GUI

### On Windows: emulator.cmd

Double-click `emulator.cmd` in the repository folder, or run it from a terminal:

```powershell
.\emulator.cmd [-Bin FILE] [-Elf FILE] [-ProfileFile FILE] [-Port N] [-Offline | -Internet]
```

It runs `run-gui.ps1` with PowerShell's script policy relaxed for that one run,
which runs `run-gui.sh` inside WSL. The first start builds the emulator, which
takes a minute; later starts take a few seconds. Your browser opens as soon as the
GUI answers. Press Ctrl+C or close the window to stop it. When it stops with an
error, the window stays open so you can read why.

If the GUI is already running on that port, the launcher opens the browser on it
and exits. If another program uses the port, it says so; start with another
`-Port`.

Run `.\emulator.cmd -Install` once to add the command `esp8266emu`, which starts the
emulator from any folder and takes the same parameters. It puts a two-line
`esp8266emu.cmd` into `%LOCALAPPDATA%\Microsoft\WindowsApps`, a folder that is on
your PATH already; delete that file to remove it.

| Parameter | Default | Meaning |
| --- | --- | --- |
| `-Bin`, `-Elf`, `-ProfileFile` | none | Windows paths, passed to `--bin`, `--elf` and `--profile-file` |
| `-Port` | `8266` | Port on 127.0.0.1 |
| `-Offline`, `-Internet` | neither | Passed as `--offline` and `--internet` (see [the network in the GUI](#the-network-in-the-gui)) |
| `-Workdir` | `~/.cache/esp8266-emulator` in WSL | Passed to `--workdir`; a path that starts with `/` is a WSL path |
| `-Distro` | `Ubuntu` | The WSL distribution that runs the emulator; list yours with `wsl -l -q` |
| `-NoBrowser` | off | Do not open the browser |
| `-NetProfile`, `-NetCapture` | none | Passed to `--net-profile` and `--net-capture`. A path that starts with `/` is taken as a WSL path, so a profile made in Ubuntu works as is. |
| `-NetForward`, `-NetHost` | none | Passed to `--net-forward` and `--net-host`; separate several with commas: `-NetForward 8080:80,8081:81` |
| `-Install` | | Add the `esp8266emu` command, then exit |

`run-gui.ps1` takes the same parameters if you prefer to start it from PowerShell.
It does not offer `--speed`. Saved state lives in your WSL home folder, which
Windows Explorer shows at
`\\wsl$\Ubuntu\home\<linux-user>\.cache\esp8266-emulator`.

### From a Linux or WSL shell

```sh
bash run-gui.sh [options]
```

The script builds the emulator, then starts the local server and prints:

```text
ESP8266 emulator GUI: http://127.0.0.1:8266/  (loopback only; Ctrl+C to stop)
virtual network: on for a .bin dropped with its .elf (Arduino core 3.1.2/lwIP2); local only: every name leads to this PC
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--bin FILE` | none | Boot this image at startup. Without it the page waits for you to drop a file. |
| `--elf FILE` | none | Matching ELF for `setup()`/`loop()` tracking and the virtual network |
| `--profile-file FILE` | none | Load a [GPIO profile](#8-gpio-profiles) and make it the default view |
| `--port N` | `8266` | Port on 127.0.0.1 |
| `--speed X` | `1` | Initial speed: `1` real time, `0` as fast as possible. Networked images run at `1` or slower. |
| `--workdir DIR` | `~/.cache/esp8266-emulator` | Where uploaded images and saved flash are kept |
| `--offline` | off | Never use the virtual network |
| `--internet` | off | Let networked images reach the internet as well as this computer |
| `--net-forward HOST:GUEST` | guest 80 on the first free port from 8080 | Forward `127.0.0.1:HOST` to port GUEST of the guest, for example `8080:80` for its web server. Repeat for more ports, up to 16. |
| `--net-host NAME=IP` | none | Answer the guest's DNS query for NAME with IP instead. Up to 16. |
| `--net-capture FILE` | none | Write the guest's Ethernet frames to a PCAP file. It starts over whenever the GUI starts a new emulator process (an upload or **Erase flash**). Captures can contain credentials. |
| `--net-profile FILE` | none | Run only the images this [network profile](#virtual-network-experimental) binds on the network |

Ctrl+C stops the server. The emulated chip is shut down cleanly and its flash is
saved.

### The network in the GUI

At startup the server builds `build/esp8266emu-net`, the network runner. That
needs libslirp:

```sh
sudo apt install -y libslirp-dev libglib2.0-dev pkg-config
```

Without it the GUI runs as before, prints how to install it, and the
[Network](#network) card says the same.

With the runner, every image you load **with its ELF** is checked. If it is an
Arduino core 3.1.2/lwIP2 image, it runs on the virtual network; otherwise it runs
offline and the Network card says why. A `.bin` without its `.elf` always runs
offline, because the network adapter finds its hooks through the ELF's symbols.
A [saved flash](#boot-with-saved-settings) dropped with them keeps the network:
the check and the profile use the plain `.bin`, and the runner finds that build at
the start of the flash. The GUI writes the network profile for you, to `network-profile.txt` in the work
directory. It covers the image and up to seven other uploaded images that pass,
so an OTA update from one to another keeps its network.

The network is **local only** unless you start with `--internet`:

- Every host name the firmware looks up resolves to your computer, `10.0.2.2`,
  so its broker, update server and API calls go to programs on your computer.
  `--net-host NAME=IP` gives a name another address.
- Packets for any other network are dropped. Nothing the firmware sends reaches
  your LAN or the internet, even if it uses an IP address directly.
- Every service that listens on your computer's loopback interface is reachable
  from the firmware.

The guest's port 80 is forwarded to the first free port from 8080; the Network
card links to it. `--net-forward` replaces that default. Live networking needs a
positive speed, so **max** is off while an image runs on the network, and an
upload switches the speed to `1×` if **max** was selected.

`--net-profile FILE` switches the automatic profile off: then only the images that
profile binds run on the network, each checked against the SHA-256 in it. The
network stays local only unless you add `--internet`. A profile must come from
`scripts/network_profile.py` run in WSL, so that the image paths in it are Linux
paths:

```sh
bash run-gui.sh --net-profile /tmp/network-profile.txt --net-forward 8080:80
```

### More than one emulator

One server runs one emulated chip, and every browser tab that opens it shows the
same chip. To run two images side by side, start a second server with its own
port and work directory, so their saved flash files cannot collide:

```sh
bash run-gui.sh --port 8267 --workdir /tmp/esp-second --bin /path/to/other.bin
```

## 6. Use the GUI

### Load firmware

- Drag the `.bin`, together with its `.elf`, anywhere onto the page.
- Or click **Choose files…** and select both files.
- To switch to a different image later, click the image name in the header, or
  drop new files.

Drop both files in one go: an ELF dropped on its own is rejected, and a `.bin`
dropped on its own runs without an ELF and without the network. The chip boots immediately. Uploads are
limited to 16 MiB. Loading the exact same image again resumes its saved flash; a
rebuilt image starts with fresh flash (see [section 7](#7-resets-flash-and-saved-state)).

#### Boot with saved settings

Firmware that keeps its Wi-Fi, broker or token settings in flash (EEPROM
emulation, a file system) boots into its own setup mode when the flash is empty.
To boot it already set up, drop a **saved flash** as a third file, together with
the `.bin` and `.elf`:

- **Name rule:** a file whose name ends in `.flash`, `.img` or `.dump` is a saved
  flash, never the image. It needs the `.bin` it belongs to in the same drop.
  **Download flash** saves files as `<image name>.flash`, so they can be dropped
  again as they are.
- It must be a **whole flash**, as the emulator keeps it: first byte `0xE9`, and
  exactly the flash size in the `.bin` header, for example 2 MB (2,097,152 bytes)
  for a 2 MB build. Anything else is refused, with the reason in the message.
- It replaces the image's saved flash, and the image chip shows `saved flash`.
  **Erase flash** still starts over from the plain `.bin`.

A saved flash comes from **Download flash** in an earlier session, or from a
board: `esptool.py read_flash 0 0x200000 board.flash` for a 2 MB board. A board
dump holds that board's real credentials (see
[privacy](#13-privacy-and-local-files)). You can also make one: pad the `.bin`
with `0xFF` bytes to the flash size and write your settings at their offset.

The flash should start with the same `.bin`. If it holds another build (after an
OTA update, say) or a header that esptool rewrote when it flashed the board, a
toast says so. The chip boots what the flash holds, and on the
[virtual network](#the-network-in-the-gui) the runner halts with
`network profile has no matching application` unless that exact build was loaded
before with its `.elf`.

### Page layout

The header carries the image chip, the profile menu and the live readouts. Below
it, the left column holds the **Board**, **Boot**, **Signals** and **Network**
cards, and the **Serial console** fills the right. In a narrow window the cards
stack: Board first, then the console, then the rest.

| Header item | Meaning |
| --- | --- |
| Image chip | File name, size, first 8 hex digits of the SHA-256, flash size, `+elf` when an ELF is loaded, and `saved flash` when flash was resumed from an earlier session or loaded from a dropped `.flash` file. Click it to load another image. |
| Profile menu | Choose the GPIO display profile |
| `emulated` | Emulated time since the last reset |
| `speed` | Measured ratio of emulated time to real time, with a sparkline of the last 20 seconds; `max · N×` in maximum-speed mode. The `1×`, `¼×` and `max` buttons beside it set the speed; `max` is dimmed while the image runs on the virtual network. |
| `MIPS` | Million guest instructions per host second, with a sparkline |
| `boot` | Boot number since the image was loaded or the flash erased; every reset adds one |
| Stage pill | Current boot stage, red `halted` if execution stopped, `stopped` if the emulator process ended, `disconnected` if the page lost contact with the server |

### Board

The Board card draws an ESP-12F module with its 22 pads where they are on the real
part: RST, ADC, EN, GPIO16, GPIO14, GPIO12, GPIO13 and VCC down the left edge; the
flash bus (GPIO11 CS0, GPIO7 MISO, GPIO9, GPIO10, GPIO8 MOSI, GPIO6 SCLK) along
the bottom; TXD0 (GPIO1), RXD0 (GPIO3), GPIO5, GPIO4, GPIO0, GPIO2, GPIO15 and
GND up the right edge. The **Reset**, **Power cycle** and **Erase flash** buttons
in the card title act on the chip (see [section 7](#7-resets-flash-and-saved-state));
Erase flash asks for confirmation first. **Download flash** saves the emulated
flash as a `.flash` file, to [drop again later](#boot-with-saved-settings).

Each GPIO pad shows its level by its fill and its mode by the tag under its name:

| Pad | Meaning |
| --- | --- |
| Green fill | HIGH |
| Dark fill | LOW |
| Dashed outline | Input without pull-up, floating |
| Amber outline | Driven from outside (see below) |
| Hatched | Flash bus, GPIO6–11; cannot be driven |

| Mode tag | Meaning |
| --- | --- |
| `OUT` | Output driven by the firmware |
| `IN↑` | Input with pull-up |
| `IN~` | Input without pull-up; floating unless driven |
| `FN` | UART or another peripheral function |

The role after the mode tag comes from the profile (`relay`, `led`, `button`,
`status led`, numbered when a profile has several groups), plus `TXD0`/`RXD0` on
GPIO1/GPIO3. Hover a pad, or reach it with Tab, for its role, mode, level, edge
count since the image was loaded, and drive state. The edge count covers every
level or mode change.

**Click a pad** to drive the pin from outside: the first click drives it LOW, the
second HIGH, the third releases it (Enter or Space does the same on a focused pad).
A driven pad has an amber outline. GPIO6–11 form the flash bus and cannot be driven.

An external drive stays in place across **Reset**. **Power cycle** releases every
external drive and clears the amber outlines.

With a [GPIO profile](#8-gpio-profiles) a faceplate appears above the module, with
one group per button, relay and LED:

- **Lamp:** the relay output. It is lit when the relay pin is at the level chosen
  under *relay drawn ON when its pin is LOW / HIGH*. This is a display setting
  only, remembered by your browser.
- **Round pad:** the button. Press and hold it to drive the button GPIO to its
  active level (LOW unless the profile says otherwise); releasing it releases the
  pin. While you hold, the faceplate shows `GPIO0 held LOW for 1.2 s (emulated)`,
  and afterwards `released after … s emulated`, which helps when testing
  long-press features.
- **Pad ring:** glows green when the LED pin is an output at its active level.
- **Small dot at the top:** the status LED, if the profile defines one.

A group is dimmed and its relay reads `unused` until the firmware configures the
relay pin as an output. How the firmware reacts to a press is up to the firmware;
the profile only maps pins.

### Boot

The timeline shows how far the current boot has progressed:
ROM → eboot → app → SDK → setup() → loop(). Reached stages are blue, the current
one green, and the time under each stage is measured from reset (hover for more
precision). The card title gives the boot number and reset reason.

| Stage | Meaning |
| --- | --- |
| ROM | The emulated ROM printed its banner and is loading the image |
| eboot | The image's entry point is in the bootloader area (Arduino eboot) |
| app | Application code has started |
| SDK | The SDK task loop is running |
| setup() | First call of `setup()`; needs the ELF |
| loop() | First call of `loop()`; needs the ELF |

Under the timeline, one chip per boot since the image was loaded shows its number
and reason: `PWR` power on, `RST` external reset, `SW` software, `WDT` watchdog.
The current boot is outlined; hover a chip for the emulated time of that reset.

| Readout | Meaning |
| --- | --- |
| loop() | `loop()` calls this boot with an ELF; loop-task runs without one |
| UART0 | Baud rate calculated from the firmware's divider (115200 shows as 115273) and bytes sent |
| CPU | Current CPU clock in MHz and instructions executed this boot |

If the emulator stops executing the firmware, a red box shows the reason; see
[section 11](#11-debug-firmware-that-does-not-boot).

### Signals

The Signals card is a logic-analyzer view of recent pin edges, each timestamped by
the emulator in emulated time. A pin appears when its level first changes (up to
six automatically); the `GPIOn` chips above the trace carry each pin's edge count
and show or hide it. Pick the window with `100 ms`, `1 s`, `10 s` or `1 min`; the
right edge is now. Blue vertical lines mark reboots. Hover the trace for the time
at the cursor and the level of every shown pin at that moment. The flash bus,
GPIO6–11, is never shown.

The window measures emulated time, so at `max` speed it scrolls faster than real
time.

### Network

The Network card shows what the SDK's own serial messages say about Wi-Fi. When
the image runs on the virtual network ([the network in the GUI](#the-network-in-the-gui)),
it also shows what the network runner reports:

| Badge | Meaning |
| --- | --- |
| soft AP | `-` until the SDK reports it; green with its IP when up (`add if1`, `dhcp server start`, `AP started`); `down` after `del if1` or `bcn 0` |
| station | Offline runs only. Idle, scanning or connection state from `scandone`, `connected with`, `ip:` and similar lines. On the virtual network the SDK prints none of these for the virtual connection, so **virtual station** takes this badge's place. |
| scans | Wi-Fi scans this boot |
| host network | Offline runs only, and why the image is offline. `offline: drop the .elf together with the .bin…` when you dropped no ELF; `offline: not an Arduino core 3.1.2/lwIP2 network image (…)` with the check that failed; `offline: the network runner is not built…` without libslirp; `unavailable: no host network bridge` with `--offline`. With `--net-profile`: `offline: this image (sha256 …) is not in the network profile`, or `offline: network profile unusable (…)`. Hover for the whole text. |
| virtual station | Virtual network only. `starting`, then `waiting for the application` until the runner matches the profile at the application stage, then `not connected` or green `connected`. `halted` when execution stopped; `stopped: …` with the runner's last message when it exited, for example because the BIN named in the profile was rebuilt. |
| IP | The guest's address once connected, normally `10.0.2.15` |
| packets | Ethernet frames the guest sent (`tx`) and received (`rx`), and how many were dropped |
| guest :PORT | One per forwarded port, by default the guest's port 80: a link to `http://127.0.0.1:HOST/`, which opens that guest port in a new tab |
| internet | `off: every name leads to this PC (10.0.2.2)` while the network is local only; absent with `--internet` |

Hover a badge for the MAC addresses. The badges stay `-` if the firmware prints
none of these messages. The runner's badges are refreshed twice a second.

While the virtual network runs, **max** in the header is off: live networking
needs a positive speed. Clicking it says so, and the server refuses speed 0 as
well. If **max** was selected when you load an image that runs on the virtual
network, the GUI switches to `1×` and says so in the emulator log.

On the local-only network, the firmware's broker, update server and API host all
resolve to your computer. To test MQTT or an OTA update, run that service on your
computer, for example a broker on port 1883 or 8883 and a web server holding the
update image. Firmware that needs settings stored in flash before it joins Wi-Fi
boots with them from a [saved flash](#boot-with-saved-settings).

### Serial console

The console shows UART0 output, with UART1 in violet. Values that look like
credentials are replaced with `*` (see [section 13](#13-privacy-and-local-files)).
Bytes are shown whatever the baud rate, so the ROM's boot messages, which a serial
monitor usually shows as garbage, are readable. A line such as
`──── boot 2: external reset ────` marks each reboot.

| Control | Effect |
| --- | --- |
| off-wire output | Shows, dimmed, bytes the firmware wrote to a UART while its TX pin was assigned to another function. Untick it to see only what a USB-serial adapter would receive. |
| emulator log | Shows the emulator's own messages in italics, such as unimplemented ROM calls, halts and ELF problems |
| follow | Keeps the console scrolled to the newest output |
| Clear | Clears the view only; the firmware is not affected |

To **send serial input**, type in *Send text to UART0 RX* and press Enter or
**Send**. The text is followed by CR LF. Long lines are cut at 512 characters,
including the line ending, and characters outside Latin-1 become `?`.

A page opened while the emulator is already running receives the recent console
history.

## 7. Resets, flash and saved state

| Action | Reset reason | RTC memory | RAM | External pin drives | Emulated flash |
| --- | --- | --- | --- | --- | --- |
| Reset button or `reset` command | 2, external | Kept | Cleared | Kept | Kept |
| Power cycle button or `power` command | 1, power on | Cleared | Cleared | Released | Kept |
| Erase flash button | 1, power on | Cleared | Cleared | Released | Rewritten from the image |
| Hardware watchdog | 4, watchdog | Kept | Cleared | Kept | Kept |

The GUI saves the emulated flash about every half second while the firmware
changes it, and again when the emulator stops. It holds everything the firmware
stored: SDK and Wi-Fi configuration, EEPROM emulation, file systems and anything
else. The next time the same image is loaded, the saved flash is used and the image
chip shows `saved flash`.

Images are identified by their SHA-256. A rebuilt firmware, even one differing by a
single byte, gets a new identity and starts with fresh flash. The old saved flash
stays in the work directory until you delete it.

To keep a state, or move it to another computer or work directory, click
**Download flash** in the Board card. It saves the whole emulated flash as
`<image name>.flash`, as last saved, so at most half a second old; before the
firmware has written anything, that is the image padded with `0xFF`. Drop the file
later with the same `.bin` (and `.elf`) to boot from it: it replaces the saved
flash for that image ([boot with saved settings](#boot-with-saved-settings)).
**Erase flash** still rewrites the flash from the plain `.bin`.

| File in `<workdir>/images/` | Contents |
| --- | --- |
| `<sha256>.bin` | The uploaded image |
| `<sha256>.flash` | Saved flash state for that image |
| `<sha256>.json` | Image name, size, profile and ELF location |
| `<sha256>.elf` | An uploaded ELF, named by its own hash |
| `<sha256>.flash-upload` | A dropped saved flash, named by its own hash, until its `.bin` arrives; at most one |

To forget everything, stop the server and delete the work directory.

## 8. GPIO profiles

A profile names your board's buttons, relays and LEDs so that the GUI can draw a
faceplate and label pins. It changes only the display and the pads' input mapping;
it never changes the firmware or its pin configuration. Start from
[the example](../examples/gpio-profile.json) and replace the pin numbers with your
board's wiring:

```json
{
  "name": "gpio-demo",
  "title": "Example GPIO panel",
  "kind": "switch",
  "gangs": [
    { "button": 0, "relay": 5, "led": 2, "button_active_low": true, "led_active_low": true }
  ],
  "status_led": null
}
```

| Field | Required | Meaning |
| --- | --- | --- |
| `name` | Yes | Identifier: 1–64 letters, digits, `-` or `_`, starting with a letter or digit. `generic` is reserved. |
| `title` | No | Display name, 1–80 printable characters; defaults to `name` |
| `kind` | No | Faceplate style: `generic` (default), `switch` or `plug` |
| `gangs` | No | Up to 8 groups. Each needs `button`, `relay` and `led` GPIO numbers. |
| `button_active_low`, `led_active_low` | No | Per group; default `true`. A pad drives LOW when `button_active_low` is true, otherwise HIGH. |
| `status_led` | No | GPIO number of a status LED, or `null` |
| `status_led_active_low` | No | Default `true` |

GPIO numbers must be 0–16. Unknown fields are rejected and the file may be at most
64 KiB. An invalid profile stops the launcher with a message such as
`server.py: error: group 1 relay must be a GPIO number from 0 through 16`.

Load it with `--profile-file FILE` (Linux) or `-ProfileFile FILE` (Windows). The
profile menu then offers it alongside `generic`. Switching profiles changes the
display immediately without restarting the firmware, and a profile picked in the
menu is kept for the next image you upload.

## 9. Run headless

The runner `build/esp8266emu` works without the GUI, for scripts, CI and terminal
use. Run it from the repository root.

### A first run

```sh
build/esp8266emu --flash /path/to/firmware.bin \
  --speed 0 --max-seconds 10 --no-stdin --quiet
```

This runs 10 emulated seconds as fast as possible and prints a summary on stderr.
For [examples/hello](../examples/hello) it looks like this:

```text
stage rom at 0 us
stage eboot at 0 us
stage app at 10 us
stage sdk at 126572 us
end: stage sdk, 10000228 us virtual, 800018294 insn, loop() 0, loop task 1697750
```

About 30 `log: phy romfunc[N] called …, returns 0` lines appear among these. They
are the SDK's radio calibration calls, and they are expected because there is no
radio. Add `--sym-setup` and `--sym-loop` (see the table below) to see the
`setup` and `loop` stages and count `loop()` calls.

### Serial monitor in the terminal

```sh
build/esp8266emu --flash /path/to/firmware.bin --quiet --uart-log /dev/stdout 2>/dev/null
```

This runs in real time and prints raw, **unmasked** UART output; `2>/dev/null`
hides the stage summary that `--quiet` writes to stderr. You can still type the
commands listed below, for example `reset`. Ctrl+C ends it.

### Options

`build/esp8266emu` without arguments prints the basic options. The debugging
options at the end of this table are accepted as well.

| Option | Default | Meaning |
| --- | --- | --- |
| `--flash FILE` | required | Image to boot |
| `--chip NAME` | `esp8266` | Chip identity and default flash capacity; see [chip and flash options](#chip-and-flash-options) |
| `--flash-size BYTES` | from header, or the `--chip` size | Override the flash size; decimal or `0x` hex, such as `0x400000` |
| `--flash-id HEX` | derived from size | 24-bit JEDEC ID that the flash chip reports |
| `--flash-out FILE` | none | Save flash writes to FILE periodically and at exit. To resume later, pass the same file as both `--flash` and `--flash-out`. |
| `--speed X` | `1` | `1` real time, `0` as fast as possible, other values scale (`0.25`) |
| `--max-seconds S` | no limit | Stop after S emulated seconds, counted from start across resets |
| `--until-loops N` | none | Stop once `loop()` has run N times in total; needs `--sym-loop` |
| `--sym-setup ADDR`, `--sym-loop ADDR` | none | Addresses of `setup()`/`loop()`, from `scripts/elfsyms.py` |
| `--script FILE` | none | Timed commands; see [section 10](#10-scenario-scripts) |
| `--report FILE` | none | JSON summary written at exit |
| `--uart-log FILE` | none | Raw copy of all UART output |
| `--no-stdin` | off | Do not read commands from stdin |
| `--quiet` | off | No JSON event stream; short summary on stderr |
| `--strict` | off | Halt at the first unimplemented ROM entry |
| `--watch ADDR=NAME` | none | Report calls of a firmware function; repeatable |
| `--calltrace FROM:TO` | none | Log calls between two emulated times in µs to stderr |
| `--mmiotrace LO:HI` | none | Log peripheral register accesses in a hex address range to stderr |
| `--trace` | off | Verbose exception and interrupt logging |
| `--profile FILE` | none | Write a sampled PC histogram at exit |
| `--net-profile FILE`, `--net-forward`, `--net-host`, `--net-local`, `--net-capture` | none | [Virtual network](#virtual-network-experimental); `build/esp8266emu-net` only |

The runner continues until `--max-seconds` or `--until-loops` is reached, a `quit`
command arrives, or stdin closes while commands are being read. With `--no-stdin`
and no limit it runs until killed. Ctrl+C does not write the `--report` file and
can lose the most recent flash writes, so prefer `--max-seconds` or `quit`.

The exit status is `0` whenever a run ends, **including a halt**, and `2` for
invalid options or an unreadable image or script. Check `"end"` in the report to
tell `quit` from `halt`.

### Commands on stdin

| Command | Effect |
| --- | --- |
| `in PIN LEVEL` | Drive GPIO PIN `0` LOW or `1` HIGH; `-1` releases it |
| `rx UART HEX` | Inject received bytes, up to 1,023 per command; `rx 0 68656c6c6f0d0a` sends `hello\r\n` |
| `reset` | External reset |
| `power` | Power cycle |
| `speed X` | Change speed |
| `pins` | Print the current pin state now |
| `trace` | Toggle `--trace` |
| `quit` | Stop, save flash and write the report |

`softreset`, `flashw ADDR FILE` and `rtcw OFFSET HEX` also exist. The OTA
rehearsal tool uses them to stage an update.

To turn text into hex for `rx`:

```sh
python3 -c 'print(b"status\r\n".hex())'
```

Commands piped in are applied as soon as they are read, so their emulated timing
is not repeatable. Use a [scenario script](#10-scenario-scripts) when timing
matters.

If the firmware halts while the runner is reading stdin without `--max-seconds`,
the runner waits for further commands, such as `reset`.

### Event stream

Without `--quiet`, stdout carries one JSON object per line. These lines come from a
synthetic image, shortened:

```text
{"t":"pins","lv":"11111111111111101","md":"pupuppuuuuuuuuuuz","vt_us":0,"t_us":0}
{"t":"stage","s":"rom","boot":1,"reason":1,"vt_us":0,"t_us":0,"insn":0}
{"t":"stage","s":"app","boot":1,"reason":1,"vt_us":0,"t_us":0,"insn":0}
{"t":"uart","u":0,"w":1,"d":"0d0a20657473204a616e20203820323031332c..."}
{"t":"stat","vt_us":1000,"t_us":1000,"insn":80000,"mips":28.3,"stage":"app","boot":1,...}
```

| Type | Main fields | Meaning |
| --- | --- | --- |
| `uart` | `u`, `w`, `d` | UART number, whether the TX pin was routed to the pad, bytes in hex |
| `pins` | `lv`, `md` | 17 levels (`0`/`1`) and modes for GPIO0–16: `o` output, `p` input with pull-up, `z` input without pull-up, `u` other function |
| `edges` | `e` | `[time_us, pin, level, mode]` for every change |
| `stage` | `s`, `boot`, `reason` | Boot stage: `rom`, `eboot`, `app`, `sdk`, `setup`, `loop` or `halted` |
| `stat` | counters | Periodic speed, counters and baud rates |
| `log` | `m` | Emulator message |
| `halt` | `m`, `pc` | Why execution stopped |
| `call` | `fn`, `n`, `a2`–`a4`, `ra` | A `--watch` hit |
| `net` | `bound`, `image`, `connected`, `ip`, `tx`, `rx`, `tx_rejected`, `rx_dropped` | With `--net-profile` only, twice a second: whether the profile is bound (and which image), the station state, its address and packet counts |

`vt_us` counts from the last reset and `t_us` from emulator start. See
[architecture](ARCHITECTURE.md#headless-protocol) for details.

### Report file

`--report FILE` writes a JSON summary at exit. This one is from 2 emulated seconds
of [examples/hello](../examples/hello) with `--sym-setup` and `--sym-loop`:

```json
{
  "end": "quit",
  "halt": "",
  "stage": "loop",
  "boots": 1,
  "virtual_us": 2000047,
  "instructions_since_boot": 160003773,
  "loop_task_dispatches": 322138,
  "setup_calls": 1,
  "loop_calls": 322137,
  "loop_calls_all_boots": 322137,
  "uart0_bytes": 467,
  "uart1_bytes": 0,
  "uart0_bytes_on_wire": 467,
  "cpu_mhz": 80,
  "flash_size": 4194304,
  "chip": "esp8266",
  "flash_id": 1458415
}
```

`end` is `quit` or `halt`, `halt` holds the halt message, and `virtual_us` and the
per-boot counters refer to the last boot. With `--net-profile`, the report also
carries a `network` object with the station state and packet counts.

### Chip and flash options

By default the runner behaves as a plain ESP8266 and takes the flash size from the
image header. `--chip` selects a module or package instead:

| `--chip` | Flash capacity | Can `--flash-size` change it? |
| --- | --- | --- |
| `esp8266` | 4 MiB (only when `--chip` is given; otherwise the header decides) | Yes |
| `esp12e`, `esp12f` | 4 MiB | Yes |
| `esp8285` | 1 MiB embedded | No |
| `esp8285h16` | 2 MiB embedded | No |

`--flash-size` accepts a power of two from 256 KiB to 16 MiB, and an image larger
than the capacity is refused. `--flash-id HEX` sets the JEDEC ID that the
firmware reads from the flash chip. These options set identity and capacity
only. They do not move addresses compiled into the image, so use the capacity
the firmware was linked for. The GUI always uses the image header.

### Virtual network (experimental)

The default build has no network. A second runner, `build/esp8266emu-net`, gives
firmware built with **Arduino core 3.1.2 and lwIP2** a Wi-Fi station connection.
The guest's own TCP/IP stack runs unchanged. The emulator hands its Ethernet
frames to a user-mode NAT on your host (libslirp), so the firmware can use DHCP,
DNS, MQTT and HTTP. Point it at test services on your own computer. There is no
radio, WPA, soft AP or scanning.

In the GUI all of this is automatic: drop the `.bin` with its `.elf`
([the network in the GUI](#the-network-in-the-gui)). The steps below run the
network headless.

1. Install the extra packages and build the network runner:

   ```sh
   sudo apt install -y libslirp-dev libglib2.0-dev pkg-config
   make network
   ```

2. Make a network profile from each image and its **matching** ELF. The profile
   binds the adapter to that exact build, so regenerate it after every rebuild:

   ```sh
   python3 -I scripts/network_profile.py --core-version 3.1.2 \
     --image /path/to/firmware.bin /path/to/firmware.elf \
     --out /tmp/network-profile.txt
   ```

   Repeat `--image BIN ELF` to cover an OTA update image as well.

3. Run with a positive speed; `--speed 1` is real time:

   ```sh
   build/esp8266emu-net --flash /path/to/firmware.bin \
     --net-profile /tmp/network-profile.txt --speed 1 --max-seconds 30 \
     --no-stdin --report /tmp/network-report.json
   ```

   Add `--net-local` to keep it on your computer, as the GUI does.

The guest gets address `10.0.2.15`. From the guest, your host is `10.0.2.2` and the
DNS server is `10.0.2.3`.

| Option | Effect |
| --- | --- |
| `--net-forward 8080:80` | Forward `127.0.0.1:8080` on the host to port 80 of the guest, for example to open its web server |
| `--net-host broker.example.test=10.0.2.2` | Answer the guest's DNS query for that name with that address; up to 16 names |
| `--net-local` | Local only: every other name resolves to `10.0.2.2`, this computer, and packets for any other network are dropped |
| `--net-capture /tmp/net.pcap` | Record every Ethernet frame for Wireshark. Captures can contain credentials. |

`run-gui.sh` takes `--net-forward`, `--net-host` and `--net-capture` as well; it
uses `--net-local` unless you pass `--internet`.

Network runs depend on host timing, so they are not repeatable the way offline
runs are. [Virtual networking](NETWORKING.md) describes the design and what has
been tested.

## 10. Scenario scripts

A scenario file feeds timed commands to the emulator, so a test presses the same
button at the same emulated moment every run. Each line is
`<emulated milliseconds> <command>`. Times count from emulator start, not from the
last reset, and may have decimals. Lines starting with `#` are ignored. Keep the
lines in time order.

```text
# Press GPIO4 for 150 ms, send a serial command, then reset.
1000   in 4 0
1150   in 4 -1
2000   rx 0 7374617475730d0a
3000   reset
```

`rx 0 7374617475730d0a` sends `status\r\n`. Any stdin command can be used.
[scenarios/gpio0-pulse.txt](../scenarios/gpio0-pulse.txt) is a two-line example.
Pick pins and timing that suit your firmware; no command has a built-in
application meaning.

```sh
build/esp8266emu --flash /path/to/firmware.bin --speed 0 --max-seconds 5 \
  --no-stdin --script scenario.txt
```

Each command is logged as a `script: …` event when it runs. Commands are applied
at the emulator's host-service points, so the actual time can be slightly later
than requested; `edges` events and reports record the actual time.

## 11. Debug firmware that does not boot

Work through these steps in order:

1. **Find the last stage reached.** The Boot timeline (GUI) or `stage` lines
   (headless) show where the boot stopped. Without an ELF, SDK main loop is the
   normal final stage.
2. **Read the console.** Tick **emulator log** to see the emulator's own messages
   alongside the firmware output.
3. **Look for unimplemented ROM calls.** A line like
   `unimplemented ROM entry 4000xxxx called from 4020xxxx: returning 0` means the
   firmware used a ROM service the emulator lacks and received 0. If behavior goes
   wrong soon after, this is a likely cause. Run headless with `--strict` to halt
   at that exact point. Radio calibration calls are expected for SDK firmware,
   because there is no radio. They appear in headless output as
   `phy romfunc[N] called …, returns 0` and in `evidence.py` reports as
   `phy_romfunc_slots_called`. The GUI hides them.
4. **Check repeated boots.** Read the lines just before each `──── boot N` marker.
   Reset reason 4 means the hardware watchdog fired: the firmware ran without
   yielding or feeding the watchdog, as it would on a board.
5. **Read the halt message**, if the stage pill shows `halted`.

| Halt message | Meaning |
| --- | --- |
| `no bootable image at flash 0x0 (first byte XX, expected E9)` | Wrong file; see [section 4](#4-prepare-your-firmware) |
| `image header runs past the end of flash`, `image segment runs past the end of flash` | Truncated or corrupt image, or a `--flash-size` that is too small |
| `boot image returned to the ROM` | The image's entry function returned; unusual image layout |
| `unhandled exception cause N at epc1 …, excvaddr …` | CPU exception before the firmware installed its own handler |
| `unimplemented ROM entry … called from …` | Only with `--strict`; see step 3 |
| `ROM->firmware call nesting too deep`, `firmware returned to ROM call level … out of order`, `guest fault outside the run loop` | An emulator-level failure; please report it ([section 14](#14-troubleshooting)) |

Common Xtensa exception causes are 0 (illegal instruction), 3 (load/store error),
6 (integer divide by zero), 9 (unaligned access), 20 (instruction fetch
prohibited), 28 (load prohibited, often a null-pointer read) and 29 (store
prohibited, often a null-pointer write). Firmware that has its own handler, such as
the Arduino core, prints its usual `Exception (28):` dump in the console instead of
halting. Memory access in the emulator is more permissive than on the chip, so some
faults a board would raise do not occur.

After a halt the emulator log shows `last PCs: …`, the addresses of the last 16
call instructions executed. Look them up with
`xtensa-lx106-elf-addr2line -e firmware.elf ADDRESS`.

### Tools for deeper inspection

The commands below use `xtensa-lx106-elf-nm`, `-objdump` and `-addr2line`. These
come with the Arduino ESP8266 core and PlatformIO's `toolchain-xtensa` package;
they are not bundled here.

**Watch a function.** Find its address, then report each call with its first three
arguments (`a2`–`a4`) and return address (`ra`):

```sh
xtensa-lx106-elf-nm -C /path/to/firmware.elf | grep -i myFunction
build/esp8266emu --flash /path/to/firmware.bin --speed 0 --max-seconds 10 \
  --no-stdin --watch 0x40201234=myFunction | grep '"t":"call"'
```

Up to 64 watches are allowed, in IRAM or flash-mapped code. After the first 200
calls of a function, only every 1,024th is reported; `n` is the running count.

**Trace calls.** `--calltrace 0:2000000 2> calls.txt` writes
`T <µs> <caller> <target>` to stderr for every call within that emulated window.
Keep the window short; the output grows quickly.

**Trace peripheral registers.** `--mmiotrace 60000300:6000037f 2> gpio.txt` writes
`R|W <µs> <pc> <address> <value>` for each access in the range; this example is
the GPIO block, and `60000000:6000007f` is UART0. Combined with `--calltrace`,
only accesses inside its time window are logged.

**Verbose tracing.** `--trace`, or the `trace` command at runtime, logs CPU
exceptions to stderr and interrupt dispatch and GPIO interrupt evaluation as log
events. It is very verbose.

**Profile.** Sample where the firmware spends its time:

```sh
build/esp8266emu --flash /path/to/firmware.bin --speed 0 --max-seconds 10 \
  --no-stdin --quiet --profile /tmp/prof.txt
xtensa-lx106-elf-nm /path/to/firmware.elf > /tmp/nm.txt
python3 -I scripts/prof.py /tmp/prof.txt /tmp/nm.txt 20
```

`prof.py` prints a percentage per function, with `(waiti idle)` for time spent
waiting for interrupts, followed by the hottest addresses.

**List ROM services the firmware uses:**

```sh
xtensa-lx106-elf-objdump -d /path/to/firmware.elf > /tmp/dis.txt
xtensa-lx106-elf-nm /path/to/firmware.elf > /tmp/nm.txt
python3 -I scripts/romcalls.py /path/to/firmware.elf /tmp/dis.txt /tmp/nm.txt
```

Each line shows a ROM address, its symbol name, the number of call sites and the
first few callers. Compare it with the `unimplemented ROM entry` messages.

## 12. Reports, regression and OTA tests

> **"Profile" means several things.** See [profile](#16-glossary) in the glossary
> before mixing `--profile`, `--profile-file`, `--net-profile` and `--chip`.

### Execution reports

`scripts/evidence.py` builds the emulator, runs an image at maximum speed and
writes one JSON report:

```sh
python3 -I scripts/evidence.py \
  --bin /path/to/firmware.bin --elf /path/to/firmware.elf \
  --seconds 10 --script scenario.txt --out /tmp/boot.json
```

It prints a one-line summary such as:

```text
wrote /tmp/boot.json: stage app, setup 0, loop 0, 19 pin edges, 117 UART bytes, wall 0.5s
```

| Option | Default | Meaning |
| --- | --- | --- |
| `--bin FILE` | required | Image to run |
| `--out FILE` | required | Report to write |
| `--elf FILE` | none | Adds `setup()`/`loop()` counts and the ELF hash |
| `--seconds S` | `10` | Emulated seconds to run |
| `--script FILE` | none | Scenario script |
| `--profile-file FILE` | none | GPIO profile; pins are recorded with their roles, such as `GPIO5(relay1)` |
| `--profile LABEL` | profile name | Label stored in the report |
| `--milestone NAME` | `M1` | Label stored in the report |
| `--note TEXT`, `--source TEXT` | empty | Free text stored in the report |
| `--emu-commit SHA` | Git HEAD | Emulator source commit to record; `EMU_COMMIT` also works |

The report records the image and ELF hashes, the emulator build and command, the
scenario, the run summary, boot stages, halts, up to 2,000 pin edges, up to 200
emulator log messages and the **masked** UART0 transcript. The firmware image is
not modified.

### Regression manifests

`scripts/regression.py` reruns your images and compares each new report with a
reviewed reference, exactly and with no timing tolerance. Use it to show that the
same image still behaves identically, for example after updating the emulator. A
reference is tied to its image hash, so a rebuilt firmware stops with
`firmware hash mismatch`; record a new reference for the new build.

1. Create a reference with `evidence.py` and review its UART, stages and pin
   edges before you trust it.
2. List it in a manifest next to your fixtures. All paths are relative to the
   manifest:

   ```json
   {
     "schema": 1,
     "cases": [
       {
         "name": "boot",
         "kind": "evidence",
         "bin": "firmware/firmware.bin",
         "elf": "firmware/firmware.elf",
         "seconds": 10,
         "reference": "references/boot.json"
       }
     ]
   }
   ```

3. Run it:

   ```sh
   python3 -I scripts/regression.py \
     --manifest /path/to/fixtures/manifest.json --out /tmp/esp8266-regression
   ```

Each case prints `EXACT name` or `DIFF name` followed by the differences, and the
exit status is `0` only if every case matches. `--case NAME` (repeatable) selects
cases, and `--compare-only` compares reports already in the output directory. The
output directory must differ from your fixtures; references are never
overwritten. [Testing](TESTING.md#regression-manifest) lists every manifest field.

### OTA-copy rehearsal

`scripts/ota_rehearsal.py` checks the second half of an Arduino OTA update without
any network. It places a new image where Arduino's Updater would have staged it,
writes the eboot copy command into RTC memory and restarts. It then checks that the
running image's own eboot copied the update to 0x0 and that the new image boots.

```sh
python3 -I scripts/ota_rehearsal.py \
  --running /path/to/running.bin --running-elf /path/to/running.elf \
  --new /path/to/update.bin --new-elf /path/to/update.elf \
  --expect 'new version marker' --out /tmp/ota-raw.json
```

The running ELF must define `_FS_start`, as Arduino builds do. `--new-elf` lets the
tool confirm that the new image reached `loop()`. `--gzip` stages a compressed
update, `--expect TEXT` requires the new image to print TEXT, and `--stage-at-ms`
(default 1500) and `--seconds` (default 6) control timing. The tool prints `PASS`
or `FAIL` and exits `0` only on PASS. If the update cannot fit beside the running
image, it reports that Updater would refuse it. HTTP download and live Updater
writes are **not** exercised here; see [testing](TESTING.md#ota-copy-rehearsal).

### Live network OTA test

`scripts/network_check.py` runs a complete OTA over the
[virtual network](#virtual-network-experimental). The firmware connects to a
small local MQTT fixture, receives an update command, downloads the new image over
HTTP with its own Updater, and reboots into it. `--method web` instead uploads the
image to the firmware's web update page.

The fixture speaks only the messages of the bundled reference sketch,
[examples/network_reference](../examples/network_reference/reference.cpp), so this
test runs that sketch rather than your own firmware. `scripts/build_reference.py`
builds its A and B versions from a local Arduino core 3.1.2 and Xtensa toolchain.
[Testing](TESTING.md#mqtt-triggered-http-ota) has the full commands and pass
criteria.

## 13. Privacy and local files

- The GUI listens on **127.0.0.1 only**, so only your own computer can open it.
  Requests that use any host name other than `127.0.0.1` or `localhost` with the
  configured port are refused with `forbidden host`. The GUI is not meant to be
  shared over a network or put behind a proxy.
- The GUI console and `evidence.py` reports mask values that follow labels such as
  `password`, `pass`, `psk`, `token`, `secret` and `api key`, the same names in
  JSON fields and URL parameters, and some storage-dump formats. Masking is
  heuristic, so review output before you share it.
- These are **not** masked: the headless event stream, `--uart-log`,
  `--calltrace` and `--mmiotrace` output, and the saved `.flash` files in the work
  directory and from **Download flash**, which hold whatever the firmware stored,
  including Wi-Fi credentials.
- In a bug report, share masked reports and image hashes rather than firmware
  binaries or flash files.

## 14. Troubleshooting

| Symptom | Cause and fix |
| --- | --- |
| `gcc: not found` or `make: not found` | Install the build tools: `sudo apt install -y build-essential python3` |
| `build the emulator first: make -C …` | `gui/server.py` was started directly. Run `make`, or use `run-gui.sh`, which builds first. |
| `OSError: [Errno 98] Address already in use`, or `Port 8266 is already in use` from `emulator.cmd` | Another program, perhaps another emulator, uses the port. Start with `--port 8267` (Linux) or `-Port 8267` (Windows). If it is the emulator GUI itself, `emulator.cmd` just opens it. |
| PowerShell: running scripts is disabled | Use `powershell -ExecutionPolicy Bypass -File .\run-gui.ps1 …` |
| `wslpath failed for …` | The Windows path does not exist, or `-Distro` names a distribution that is not installed (`wsl -l -q`). |
| Browser shows `forbidden host` | Open exactly `http://127.0.0.1:<port>/` or `http://localhost:<port>/`, with the port the server printed. |
| `Upload refused: not an ESP8266 flash image (first byte must be 0xE9)…` | You dropped the wrong file. Use the image for flash address 0x0 (section 4). |
| `Choose a .bin flash image starting at address 0x0.` | Only an `.elf` was dropped. Drop the `.bin` together with it. |
| Stage pill shows `disconnected` | The server stopped or the connection dropped. Restart the server and reload the page. |
| Stage pill shows `halted` | Read the red box and see [section 11](#11-debug-firmware-that-does-not-boot). |
| Stages stop at SDK main loop | Normal without an ELF. With an ELF, check that it comes from the same build. |
| `Upload refused: not an ELF file`, or `ELF not usable: …` in the emulator log | The file is not a 32-bit little-endian ELF. Use the build's `firmware.elf`. |
| `loop()` row shows loop-task runs | No ELF was loaded. Drop the `.bin` and `.elf` together. |
| Console stops after the boot messages | The firmware may write to UART1 (violet), move TX to another pin (tick **off-wire output**), or not print at all. Tick **emulator log** for more detail. |
| Boot counter keeps climbing | The firmware keeps resetting. Check the reset reason; 4 is the watchdog ([section 11](#11-debug-firmware-that-does-not-boot)). |
| Wi-Fi never connects; station shows `scanned (no access point: the emulator has no radio)` | The image runs offline; the host network badge says why. Drop the `.bin` together with its `.elf`, and install libslirp if the badge asks for it ([the network in the GUI](#the-network-in-the-gui)). |
| Network card: `offline: drop the .elf together with the .bin…` | The network needs the ELF from the same build. Drop both files in one go. |
| Network card: `offline: not an Arduino core 3.1.2/lwIP2 network image (…)` | The image was not built with Arduino core 3.1.2 and lwIP2, or the ELF is from another build. The text in brackets names the check that failed. The image still runs, without a network. |
| Network card: `offline: the network runner is not built…`; the launcher prints `virtual network: off, libslirp is missing` | Install libslirp in WSL: `sudo apt install -y libslirp-dev libglib2.0-dev pkg-config`, then start the GUI again. |
| Virtual station stays `not connected` | The firmware has not joined Wi-Fi. Firmware that keeps its Wi-Fi settings in flash and waits in its own setup mode for them stays there: the virtual network cannot reach a soft AP. Drop a saved flash that already holds the settings together with the `.bin` and `.elf` ([boot with saved settings](#boot-with-saved-settings)). The SSID and password in it can be anything: the virtual station joins whichever network the firmware asks for. |
| `Upload refused: not a saved flash (first byte must be 0xE9)…` | The `.flash`, `.img` or `.dump` file is not a whole flash starting at address 0x0. Save it again with **Download flash**, or read the board from address 0. |
| `Upload refused: a saved flash is the whole flash: this file is N bytes, its header says …` | The file is only part of the flash. Read the whole chip, for example `esptool.py read_flash 0 0x200000 board.flash` for 2 MB. |
| `Upload refused: the saved flash is N, but this image is built for M of flash` | The flash comes from a board or build with another flash size. Drop the flash saved with this firmware, or build for the board's flash size. |
| Toast: `The saved flash does not start with this .bin…` | The flash holds another build, for example after an OTA update. It boots anyway. For the virtual network, drop it with the `.bin` and `.elf` it holds ([boot with saved settings](#boot-with-saved-settings)). |
| `HALT: network profile has no matching application…` | The flash holds a build the GUI has no `.elf` for. Load that build's `.bin` and `.elf` once, or drop a flash saved with this `.bin`. |
| `--net-profile needs the network runner …, and building it needs libslirp` | The GUI was started with `--net-profile`, but libslirp is missing. Install it: `sudo apt install -y libslirp-dev libglib2.0-dev pkg-config`. |
| Network card: `offline: this image (sha256 …) is not in the network profile` | The profile binds exact builds. Regenerate it with `scripts/network_profile.py` for this BIN and its ELF, then load the image again. |
| `live networking needs a positive speed: the network runner refuses 0 (max)` | The image runs on the virtual network; choose `1×` or `¼×`. |
| `HALT: cannot forward host port N`; virtual station shows `halted` | Another program already listens on 127.0.0.1:N. Choose a free host port for `--net-forward`. |
| `pio: command not found` | PlatformIO is not installed on this side. Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html), or build on Windows and run `run-gui.ps1` ([section 2](#your-first-run-with-the-example)). |
| `flash: image is N bytes; flash capacity is M bytes` | The file is larger than the flash size in its header or the selected `--chip`. Use the image for address 0x0, or a larger `--flash-size`. |
| `unknown chip profile: …` | Use one of `esp8266`, `esp12e`, `esp12f`, `esp8285`, `esp8285h16`. |
| `network: networking needs libslirp: build with make network` | `--net-profile` was given to `build/esp8266emu`. Run `make network` and use `build/esp8266emu-net`. |
| `Install libslirp-dev, libglib2.0-dev and pkg-config…` from `make network` | Install those packages: `sudo apt install -y libslirp-dev libglib2.0-dev pkg-config`. |
| `network options require --net-profile` | `--net-forward`, `--net-host` and `--net-capture` only work together with `--net-profile`. |
| `live networking requires a positive --speed (use 1)` | Network runs cannot use `--speed 0`. |
| **max** is greyed out | The image runs on the virtual network, which needs real time. If you do not need the network, drop the `.bin` without its `.elf` (you lose the `setup()`/`loop()` stages), or start with `-Offline` (Windows) or `--offline` (Linux). |
| The `speed` readout stays below `1.00×` | The host cannot keep up in real time. The results are still valid; use `max` for long runs. |
| Saved settings disappeared after a rebuild | A new build has a new SHA-256 and therefore fresh flash ([section 7](#7-resets-flash-and-saved-state)). |
| `evidence.py: error: --seconds must be a finite positive number` | Pass a positive `--seconds` value. |
| `firmware hash mismatch: …` from `regression.py` | The image differs from the one the reference was recorded with. Record a new reference. |

**Reporting a problem:** include the emulator commit, the image and ELF hashes,
the exact command or scenario, what you expected and what happened, and the masked
`evidence.py` report. If you can, point to the relevant source `file:line`. Keep
firmware, flash files and raw traces private unless you are allowed to share
them. See [contributing](../CONTRIBUTING.md).

## 15. Command summary

```sh
# Build and self-test
make && make check

# GUI (Linux/WSL): open http://127.0.0.1:8266/ and drop fw.bin + fw.elf on it.
# With libslirp, Arduino 3.1.2 images get a local-only network by themselves.
bash run-gui.sh [--profile-file board.json] [--port N] [--offline | --internet]

# Quick headless boot check
build/esp8266emu --flash fw.bin --speed 0 --max-seconds 10 --no-stdin --quiet

# Serial output in the terminal (raw, unmasked)
build/esp8266emu --flash fw.bin --quiet --uart-log /dev/stdout 2>/dev/null

# Scripted inputs plus a summary file
build/esp8266emu --flash fw.bin --speed 0 --max-seconds 5 --no-stdin \
  --script scenario.txt --report /tmp/report.json

# Masked, shareable execution report
python3 -I scripts/evidence.py --bin fw.bin --elf fw.elf --seconds 10 --out /tmp/boot.json

# Compare against reviewed references
python3 -I scripts/regression.py --manifest fixtures/manifest.json --out /tmp/regression

# Another chip or flash size
build/esp8266emu --flash fw.bin --chip esp8285 --speed 0 --max-seconds 10 --no-stdin --quiet

# Virtual network (experimental)
make network
python3 -I scripts/network_profile.py --core-version 3.1.2 --image fw.bin fw.elf --out net.txt
build/esp8266emu-net --flash fw.bin --net-profile net.txt --speed 1 --net-local --net-forward 8080:80
```

```powershell
# GUI from Windows: double-click emulator.cmd, or
.\emulator.cmd [-ProfileFile board.json] [-Port N] [-Offline | -Internet]

# Once: add the esp8266emu command, then start the GUI from any folder
.\emulator.cmd -Install
esp8266emu
```

## 16. Glossary

| Term | Meaning |
| --- | --- |
| Flash image, `.bin` | The complete contents of the flash chip from address 0x0, starting with the `0xE9` header byte. This is what the emulator boots. |
| ELF, `.elf` | The linker output from the same build. The emulator reads only its symbol table, for example to find `setup()` and `loop()`. |
| ROM | Code built into the ESP8266 that runs first and offers services to the firmware. The emulator implements these services in C; it contains no ROM dump. |
| eboot | Arduino's bootloader at the start of the image. It starts the application, and copies a downloaded OTA update into place first. |
| SDK | Espressif's NONOS SDK inside the firmware. It runs Wi-Fi, timers and the task loop that calls Arduino's `loop()`. |
| GPIO | A general-purpose pin, numbered 0–16. GPIO6–11 connect to the flash chip and cannot be driven. |
| UART0, UART1 | The serial ports. UART0 is the one a USB-serial adapter shows; UART1 can only transmit. |
| RTC memory | A small memory that keeps its contents across a reset but not across a power cycle. eboot reads OTA commands from it. |
| Watchdog | A hardware timer that resets the chip when firmware runs too long without yielding. Reset reason 4. |
| Emulated time | Time as the firmware sees it. At `max` speed it runs faster than the clock on the wall. |
| Scenario | A text file of timed commands, such as button presses, for repeatable runs ([section 10](#10-scenario-scripts)). |
| Reference report | A reviewed `evidence.py` report that later runs are compared with exactly ([section 12](#12-reports-regression-and-ota-tests)). |
| Profile | Five different things. A **GPIO profile** (`--profile-file`, `-ProfileFile`) names buttons, relays and LEDs for the GUI. A **chip profile** (`--chip`) sets chip identity and flash size. A **network profile** (`--net-profile`) binds the virtual network to one build. The runner's `--profile FILE` writes a CPU time histogram. `evidence.py --profile LABEL` is only a label stored in the report. |
