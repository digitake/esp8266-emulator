# Execution evidence

The public test suite uses synthetic inputs and does not require a firmware
bundle. `make check` writes its generated watchdog report under `build/`.

For firmware-level comparisons, keep your images, matching ELFs, scenarios and
reference JSON in a local fixture directory. Pass a manifest to
`scripts/regression.py`; the tool checks the input hashes recorded by your
references and compares deterministic behavior exactly. It never rewrites a
reference automatically.

Reports can contain firmware output even after known credential patterns are
masked. Review them before sharing. Record image hashes and source provenance,
retain the unmodified local reference, and keep credentials and firmware binaries
outside this public repository.

See [testing and evidence](../docs/TESTING.md) for commands and the
[manifest example](../examples/regression-manifest.json) for the fixture format.
