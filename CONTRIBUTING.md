# Contributing

Build and test in Linux or WSL Ubuntu with GCC, Make and Python 3:

```sh
make
make check
```

The tests use synthetic inputs and need no external firmware or network service.
See [testing](docs/TESTING.md) for optional firmware regression manifests and
user-CPU speed measurement.

For a bug, include the emulator commit, command/scenario, relevant `file:line`,
expected behavior and observed behavior. Share masked UART output and firmware
hashes; keep credentials, firmware binaries, flash state and raw traces local.

Emulator fixes need a concrete reproducer. Compare deterministic counts, times
and pin edges exactly; explain any intended model change before recording a new
reference. Keep the original evidence. Performance changes must preserve guest
behavior and be measured using user CPU time, separately from wall time.

Keep the GUI loopback-only and the firmware image unmodified. Use local services
for network experiments. Contributions are accepted under the project's MIT
license; retain notices for any third-party material you introduce.
