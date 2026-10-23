# Current scope

ESP8266 Emulator provides an LX106 interpreter and a partial ESP8266 peripheral
and ROM-service model. It is intended for inspecting unmodified firmware and
developing reproducible tests. Support for a CPU instruction or register does
not establish compatibility with every firmware that uses it.

## Implemented areas

| Area | Implementation |
| --- | --- |
| CPU | CALL0 instructions, density instructions, interrupts/exceptions, cycle counter and validated predecode cache |
| Boot and ROM | E9 image loading, guest callbacks, task scheduling, selected libc/libgcc and crypto services |
| I/O | GPIO0–16, IO_MUX, edge/level GPIO interrupts, UART TX/RX and FRC timers |
| Flash | NOR program/erase behavior, SPI0 operations, cache mapping and persistent image state |
| Chip profiles | ESP8266, ESP-12E/F, ESP8285 1 MiB and ESP8285H16 2 MiB identity/capacity settings |
| Virtual station | Optional libslirp NAT, guest lwIP DHCP/DNS/TCP, station events and OFF/STA reconnects for Arduino 3.1.2/lwIP2 |
| Minimal MAC | No-ACK completion for queues 0–7 through IRQ0, virtual TSF and one MAC timer |
| Watchdog | Register-controlled feed, gating, interrupt stages and reset cause 4 |
| GUI | ESP-12F pin view with input driving, boot timeline, signal trace, user-defined pin profiles, masked UART, loopback request guards, serialized process lifecycle, and the virtual network for uploaded Arduino 3.1.2 images with their ELF, local only by default |
| Tools | Masked reports, regression manifests, offline eboot rehearsal and live MQTT/HTTP or station web-upload OTA checks |

The source models were developed around Arduino core 3.1.2 and NONOS SDK
interfaces. Different SDK versions, bootloaders or firmware may exercise
unimplemented behavior. Use `--strict` to stop at unknown ROM entry points;
without it those calls log and return zero.

## Reproducible validation

`make check` requires no firmware files. It runs the C suites under AddressSanitizer
and UndefinedBehaviorSanitizer, tests GUI/tooling behavior, and builds a small
synthetic image to check watchdog resets. The predecode suite compares optimized
execution against the reference decoder, including code changes and event timing.

Firmware-level validation is optional and belongs to the fixture owner. Supply
images, matching ELFs, scenarios and reviewed JSON references in a regression
manifest. The public repository has no bundled device firmware or product-specific
golden records. See [testing](TESTING.md) for exact comparison and provenance rules.

The public reference source can be built with a separately installed Arduino
core 3.1.2/toolchain. Live tests run its own MQTT client, HTTP updater, EEPROM,
eboot and replacement application through the network adapter. Both raw and gzip
updates have passed with 4 MiB ESP-12F and 1 MiB ESP8285 profiles. These results
establish those specific builds and paths; they are not a blanket SDK guarantee.

## Remaining limits

There is no RF model or full 802.11 MAC. The optional [network adapter](NETWORKING.md)
supplies a virtual station at the lwIP/SDK boundary and requires matching BIN/ELF
profiles. It does not establish SoftAP recovery, WPA, scanning, roaming, radio
sleep or compatibility with arbitrary SDK versions. SDK AP state is not a Wi-Fi
network that another device can discover.

ROM services are source implementations, not a ROM dump. Initial ROM watchdog
setup and the complete undocumented watchdog mode API are incomplete. Memory
access is more permissive than hardware, flash operations are instantaneous,
clocks are ideal, and timing uses one cycle per instruction.

No physical-board timing or electrical behavior is claimed. The
[board protocol](BOARD-COMPARISON.md) separates real measurement uncertainty from
the exact deterministic emulator gate.
