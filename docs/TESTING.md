# Testing and evidence

Use Linux or WSL Ubuntu with GCC, GNU Make and Python 3. Run commands from the
repository root unless an example says otherwise.

## Tests without firmware

```sh
make
make check
```

The C tests use AddressSanitizer and UndefinedBehaviorSanitizer. They cover
instruction semantics, exceptions, predecode equivalence, scheduler priorities,
peripherals, MAC completion and watchdog behavior. Python tests cover masking,
GUI request handling, child-process lifecycle, runner stdin commands across
resets and command-line tools. These
checks use synthetic inputs and need no SDK, firmware bundle or network service.

LeakSanitizer is disabled for compatibility with ptrace environments; address
and undefined-behavior checks remain enabled. `make test` runs the C and Python
suites. `make check` also runs a synthetic firmware image that hangs and resets
with watchdog cause 4. Run that final check separately with:

```sh
python3 -I scripts/watchdog_check.py --out /tmp/watchdog-hang.json
```

## Your own firmware fixtures

Firmware-level regression is optional. You supply the flash images, matching
ELFs, input scenarios and reviewed reference reports. There is no built-in
firmware collection or default reference directory.

Create an initial report for an image you are permitted to use:

```sh
python3 -I scripts/evidence.py \
  --bin /path/to/firmware.bin --elf /path/to/firmware.elf \
  --seconds 10 --milestone BOOT --out /path/to/fixtures/references/boot.json
```

The ELF is optional for ordinary execution reports and provides symbol-based
Arduino function counts when available. `--script FILE` adds timed inputs;
`--profile LABEL` labels the report and `--profile-file FILE` supplies GPIO-role
metadata. These options do not alter the image.

A reference records observed behavior, not automatically correct behavior.
Inspect its UART, boot stages and pin edges before adopting it, and compare to
a board or independent expectation when available. Keep original references
when fixing a defect and explain each intended deterministic difference.

## Regression manifest

Save a JSON manifest beside your fixture files. This minimal example runs one
boot case:

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
      "milestone": "BOOT",
      "reference": "references/boot.json"
    }
  ]
}
```

All paths are relative to the manifest. The
[full example](../examples/regression-manifest.json) also shows an OTA case;
its paths are placeholders for your own files. Add `script` to an evidence case
to drive a timed-input scenario.

```sh
python3 -I scripts/regression.py \
  --manifest /path/to/fixtures/manifest.json --out /tmp/esp8266-regression
```

| Case kind | Required fields beyond `name` and `kind` | Optional fields |
| --- | --- | --- |
| `evidence` | `bin`, `reference` | `elf`, `profile`, `profile_file`, `seconds`, `script`, `milestone` |
| `ota` | `running`, `running_elf`, `new`, `reference` | `gzip`, `seconds`, `stage_at_ms`, `expect` |

Case names must be safe filename identifiers and unique within the manifest.
The parser rejects unknown keys; manifests select supported operations rather
than arbitrary shell commands. `--case NAME` is repeatable and selects a subset.
`--compare-only` compares reports already present in the output directory.

The runner checks the input hashes required by each reference. It writes one
report per case and a `comparison.json` containing every deterministic difference.
Counts, stage times, UART, pin edges and OTA results compare exactly. Only
documented provenance, editorial text, timestamps and host runtime are excluded;
there is no timing tolerance. The output directory must be separate from your
reference files, which the runner never replaces.

Git provenance is detected from the checkout. Set `EMU_COMMIT` when running
from an exported tree or when Git cannot inspect the checkout:

```sh
export EMU_COMMIT=<source-commit>
```

## OTA-copy rehearsal

The OTA tool targets the Arduino eboot copy protocol. It needs a compatible
running image, its matching ELF for the flash layout, and a replacement image:

```sh
python3 -I scripts/ota_rehearsal.py \
  --running /path/to/running.bin --running-elf /path/to/running.elf \
  --new /path/to/update.bin --expect 'new version marker' \
  --out /tmp/ota-raw.json
```

Add `--gzip` to stage a compressed update. The tool injects staged bytes and a
CRC-protected RTC command, resets the guest, and checks the image's own eboot
copy, erased tail and resulting boot. It does **not** exercise HTTP download,
live Updater writes or application-specific OTA triggering. Firmware using a
different bootloader or update protocol needs a different rehearsal.

## Performance and bug reports

Measure user CPU separately from wall time on a shared host. The following
command uses GNU `time` (the optional `time` package on Ubuntu):

```sh
/usr/bin/time -f 'user=%U s system=%S s wall=%e s' \
  build/esp8266emu --flash /path/to/firmware.bin \
  --speed 0 --max-seconds 10 --no-stdin --quiet
```

Divide the final instruction count by user CPU seconds and by one million for
MIPS. Record build flags, compiler, host, emulator commit and image hash. Repeat
runs; an idle image or ROM-service-heavy workload does not measure the same
interpreter workload as a busy application loop. A speed improvement must
preserve accepted deterministic behavior.

Bug reports should include **source file:line and a concrete failing scenario**,
image/ELF hashes, command line, expected versus actual behavior and masked
evidence. Keep credentials and firmware binaries local unless you have permission
to share them. Physical-board comparisons follow the separate
[bench protocol](BOARD-COMPARISON.md).
