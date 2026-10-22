# Licensing and references

The emulator, tools, tests and documentation in this repository are distributed
under the [MIT License](LICENSE). Firmware loaded by the emulator is supplied
separately; this license does not relicense Espressif SDKs, Arduino libraries,
firmware images, toolchains or physical mask-ROM contents.

The ROM model implements entry-point behavior in C and constructs its data
tables at runtime. This distribution contains no ROM dump, firmware binary,
Espressif SDK library or bundled QEMU implementation.

Specifications and compatibility references used during development include:

- [Espressif ESP8266 Technical Reference](https://www.espressif.com/sites/default/files/documentation/esp8266-technical_reference_en.pdf)
  for peripheral registers.
- [QEMU's Xtensa target](https://gitlab.com/qemu-project/qemu/-/tree/master/target/xtensa)
  as an ISA/exception behavior comparison reference.
- [Arduino core for ESP8266](https://github.com/esp8266/Arduino)
  for ABI layouts, MMU mappings, ROM interfaces, lwIP glue and eboot behavior.
- [NIST FIPS 197](https://csrc.nist.gov/pubs/fips/197/final) for AES arithmetic
  and substitution tables. The standalone table generator computes field inverses
  and the affine transform directly from the specification.
- [NIST FIPS 180-4](https://csrc.nist.gov/pubs/fips/180-4/upd1/final),
  [RFC 1321](https://www.rfc-editor.org/rfc/rfc1321),
  [RFC 2104](https://www.rfc-editor.org/rfc/rfc2104) and
  [RFC 8018](https://www.rfc-editor.org/rfc/rfc8018) for hash/HMAC/PBKDF2 algorithms.
- [Host AP](https://w1.fi/hostapd/) for compatible crypto interface conventions.
- [Mongoose OS watchdog experiments](https://mongoose-os.com/blog/esp8266-watchdog-timer/)
  and its register definitions for observed ESP8266 watchdog behavior.

These are references, not bundled runtime dependencies. Their projects and
publications retain their own licenses and notices.
