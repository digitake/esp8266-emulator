# Architecture

The emulator runs the firmware's instruction stream in a single host process.
The GUI is a separate local server that starts the runner, sends commands on
stdin and consumes JSON events on stdout.

```text
Browser at 127.0.0.1
        │ HTTP / server-sent events
gui/server.py
        │ stdin commands / stdout JSON
build/esp8266emu
        ├── src/cpu.c     LX106 instructions and exception entry
        ├── src/rom.c     source ROM services and guest callbacks
        ├── src/soc.c     memory, peripheral registers and events
        ├── src/crypto.c  supported ROM crypto operations
        └── src/main.c    flash loading, time, commands and reports
```

## Boot and execution

The input is an E9-format flash image intended for address zero. The source ROM
loader reads its segments and enters the image. In a compatible Arduino boot
flow, that entry is the image's own eboot, which loads the application or performs
an OTA copy first. The SDK, core and application execute from the supplied image;
the emulator does not replace application functions or input handling.

ROM calls are intercepted at their guest addresses and handled in C. Services
that need guest callbacks—interrupt dispatch, scheduled tasks and installed
console callbacks—return control through guest instructions. Unimplemented ROM
entries are logged and return zero; `--strict` halts at the first such call.
This repository contains source implementations rather than an Espressif ROM dump.

The LX106 interpreter implements CALL0 instruction and exception behavior.
A 16,384-entry predecode cache validates each
entry's PC and raw instruction bytes on use. This covers self-modifying IRAM,
flash writes and remapping without write-side invalidation. Common operations
use GCC threaded dispatch; uncommon instructions use the general decoder.
Execution blocks stop at event boundaries, MMIO stores, special operations and ROM
calls, and contain at most 32 instructions. IRQ and observer behavior remains
consistent with the instruction-by-instruction model.

## Peripherals and time

Modeled interfaces include UART0/1, GPIO0–16 and IO_MUX, FRC1/FRC2, RTC memory,
SPI0 flash, cache mapping, selected DPORT/eFuse registers and the hardware watchdog.
Flash programming only clears bits and erasing restores them. The GUI persists
each image's full flash state so SDK configuration survives a restart.

Virtual time assumes one cycle per interpreted instruction. Clock domains are
tracked for timers and the watchdog, but flash erase/program is instantaneous
and peripheral clocks are ideal. Unaligned and byte access to IRAM/flash is more
permissive than the chip. These are functional models, not cycle-accurate board
timing or electrical models.

The focused MAC model completes queues 0–7 with no ACK policy after an idealized
300 µs. It sets TX-start/TX-done status and raises IRQ0. Compatible NONOS SDK code
handles this through `wDev_ProcessFiq` and releases the frame; its NMI vector has
a separate timer role. TSF follows boot time, and one modeled MAC timer can drive
the SDK's periodic watchdog-feed work. Other SDK interrupt arrangements require
separate validation.
Special queue 10, other MAC timer channels, acknowledged unicast and RX are absent.

The hardware watchdog is controlled through `0x60000900` and related registers.
It supports feed value `0x73`, single/two-stage operation, interrupt gating and
hardware reset cause 4. It starts when firmware enables it; ROM-bootstrap
watchdog initialization and all undocumented ROM mode variants are not modeled.
Firmware must feed it through its own register writes or SDK timer/task path.

## Device profiles and GUI state

The default view has no device-specific pin roles. An optional JSON profile
describes your board's buttons, relays and LEDs for display and input; it does not
rewrite guest pin configuration or add application behavior. The GUI derives
boot stages from runner events, function counts from a matching ELF and Wi-Fi
messages from complete masked UART lines.

Pass `--profile-file FILE` to the Linux launcher or `-ProfileFile FILE` to the
Windows launcher. Start with [the example](../examples/gpio-profile.json) and
replace its pin numbers and active levels with your board's wiring:

| Field | Meaning |
| --- | --- |
| `name`, `title` | Profile identifier and display name |
| `kind` | `generic`, `switch` or `plug` presentation |
| `gangs` | Up to eight objects with `button`, `relay` and `led` GPIO numbers |
| `button_active_low`, `led_active_low` | Optional per-gang booleans, default `true` |
| `status_led` | Optional GPIO number or `null` |
| `status_led_active_low` | Optional boolean, default `true` |

GPIO numbers must be 0–16. Profiles are limited to 64 KiB; names and titles are
bounded, and `generic` is reserved by the GUI. Selecting a profile does not
restart the guest. Buttons drive their configured active level while held and
release it afterward; hold behavior belongs to the application. The pin grid
shows all 17 GPIOs and disables interactive driving of flash-bus GPIO6–11.
A display polarity setting is not proof of physical output polarity.

UART events distinguish writes to the UART peripheral from bytes that reached a
pad still configured for UART. The console dims off-wire bytes. This matters when
firmware repurposes GPIO1 after boot: its internal UART writes may continue while
a physical serial adapter would see nothing.

The server serializes process start/stop/restart, waits for a child to save flash
before erasing it, and rejects events from an earlier process generation. Uploads
are published atomically under SHA-256 filenames. Host/Origin checks and a fixed
127.0.0.1 bind keep the interface local.

With `--net-profile`, the server starts `build/esp8266emu-net` only for an image
whose SHA-256 the profile lists; any other image runs on the offline runner, so
the runner's image check never halts an upload. It turns `net` events into the
Network card's badges and refuses speed 0 while the network runner is active.

## Headless protocol

Without `--quiet`, stdout contains one JSON object per line. Each event has a
`t` type field. Frequently used types are:

| Type | Main fields | Meaning |
| --- | --- | --- |
| `uart` | `u`, `w`, `d` | UART index, pad-routed flag and hex bytes |
| `pins` | `lv`, `md` | Seventeen GPIO levels and modes |
| `edges` | `e` | Arrays of `[time_us, pin, level, mode]` |
| `stage` | `s`, `boot`, `reason`, `vt_us`, `t_us` | Boot stage and timing |
| `stat` | Counters and time | Periodic execution state |
| `log` | `m` | Diagnostic message |
| `halt` | `m`, `pc` | Stop reason and guest PC |
| `call` | `fn`, `a2`–`a4`, `ra` | Function watch observation |
| `net` | `bound`, `image`, `connected`, `ip`, `tx`, `rx`, `tx_rejected`, `rx_dropped` | Virtual station state, every 500 ms with `--net-profile` only |

Commands are newline-delimited text on stdin:

```text
in <pin> <0|1|-1>    # LOW, HIGH or release external drive
rx <uart> <hex>     # inject UART receive bytes
reset              # external reset; RTC retained
power              # power cycle; RAM/RTC cleared, flash retained
speed <factor>     # 1 = real-time pacing; 0 = maximum speed
pins
quit
```

Scenario files contain `<emulated milliseconds> <command>`. Inputs are delivered
at host-service slices; evidence records the actual virtual edge time.

Useful runner options include `--script FILE`, `--report FILE`, `--flash-out FILE`,
`--sym-setup ADDR`, `--sym-loop ADDR`, `--watch ADDR=NAME`,
`--calltrace FROM_US:TO_US`, `--mmiotrace LO:HI` and `--profile FILE`. In the
headless runner, `--profile` writes a sampled PC histogram. The GUI's
`--profile-file` selects a display layout; the evidence tool's `--profile` is a
report label. `scripts/prof.py` resolves histogram addresses
using a symbol listing, and `scripts/romcalls.py` summarizes ROM references.

The raw event stream and `--uart-log FILE` can contain firmware credentials.
GUI output and `scripts/evidence.py` reports apply the masking rules. Inspect
any additional raw trace before publishing it.
