# Comparison with a physical board

This protocol defines a future bench comparison. The emulator's deterministic
tests and hardware measurements answer different questions: keep emulator
references exact and record physical timing uncertainty separately.

## Prepare matching inputs

1. Record the module/PCB revision, chip family, crystal, flash part/size, supply,
   reset wiring and capture equipment. Record SHA-256 hashes of the exact image
   and matching ELF, emulator commit/binary hash, command, scenario and GPIO profile.
2. Use the same image on both systems. Keep debug and release builds as separate
   experiments. Back up the board's flash before changing it.
3. Match initial flash state: image followed by erased bytes for blank flash,
   or the same saved configuration for a configured run. Separate first power-on,
   subsequent reset after SDK calibration/config writes, and deliberate erase.
   Restore the chosen state before each repeat.
4. Derive GPIO roles, active levels and supported button actions from your board
   and firmware. The generic emulator view does not prescribe them. Keep
   credentials local and use an isolated test network when a board needs Wi-Fi.

## Capture and stimulus

Capture reset, UART TX, relevant inputs and outputs on one timebase. A sample
rate of at least 20 MHz helps resolve microsecond transitions. Use appropriate
logic levels and a common reference. For relay applications, measure mechanical
contacts separately from the electronic driver. A logic analyzer alone cannot
distinguish a floating HIGH from a driven HIGH; record mode as unknown unless
bias checks or analog measurements establish it.

Trigger power-on/external-reset captures on reset release. Decode the ROM banner
at the baud rate appropriate to the crystal, then use the firmware's actual UART
divider. The emulator's UART `w` flag identifies pad-routed output. Firmware may
repurpose the UART pin after boot; compare only on-wire bytes to a serial capture.

Choose scenarios that your application supports: untouched boot, external reset,
a short input pulse, repeated presses, and a long hold only if it has a defined
meaning. A timed input fixture is more repeatable than manual pressing. Record
active levels, release edges and bounce rather than assuming button semantics.
Collect at least five repeats per case.

Emulator scenario files contain `<emulated milliseconds> <command>`. Select the
correct pin for your board. For example, this pulse drives GPIO4 LOW and then
releases it; it is not a predefined application action:

```text
1000 in 4 0
1150 in 4 -1
```

Inputs are delivered at host-service slices. Compare recorded input-edge times,
not just the nominal scenario millisecond.

## Compare observations

- Match ROM banners, reset causes, boot mode and bootloader output. Distinguish
  power-on, external reset, software restart and watchdog reset.
- Compare output-edge order and levels first, then reset-to-edge and
  input-to-output delays. GUI polarity is display configuration, not a board fact.
- Use observable UART and pin milestones. ELF-based `setup()`/`loop()` watches
  are not directly measurable on a board without a suitable trace/debug probe.
- Compare flash and RTC effects across reset types. To induce a watchdog hang,
  verify that the debugger does not pause or disable the watchdog clock; a halted
  CPU is not necessarily equivalent to a running, non-yielding loop.

Archive raw captures locally with a manifest of hashes, equipment settings,
initial state, stimuli and firmware/emulator versions. Mask UART before sharing
using `mask_text` from `scripts/mask.py`, then inspect the result for other private
data. Export edges as `run_id,t_us,pin,level,mode,source` with a common time origin
and `unknown` for unmeasured modes.

Derive tolerances from sample interval, trigger uncertainty and repeat variation.
Report distributions instead of selecting one convenient run. The model uses
ideal clocks, one cycle per instruction, simplified MAC airtime and instantaneous
flash operations; exact board boot timing is not promised.

Each mismatch report should include **source file:line + concrete scenario**,
image hash, expected board observation, emulator result and both traces.
Classify it as a documented modeling assumption, a reproduced defect or an
unresolved board effect. Do not relax exact emulator regression to absorb jitter.

## OTA comparison

A real OTA test needs the application's actual update trigger, a local server
and a compatible update image. Capture the HTTP transaction, Updater staging,
boot command, bootloader result and final bytes. This will also require the
future [network bridge](NETWORKING.md) for an equivalent emulator run.

The current eboot rehearsal injects already-staged bytes and a command before
reset. It can compare copy behavior, but does not establish download, live
Updater execution or board flash timing. See [testing](TESTING.md).
