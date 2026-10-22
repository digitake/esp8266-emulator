#!/usr/bin/env bash
# ESP8266 emulator GUI on http://127.0.0.1:8266/ (loopback only). WSL/Linux: needs gcc, make, python3.
# Usage: ./run-gui.sh [--bin firmware.bin] [--elf firmware.elf] [--profile-file examples/gpio-profile.json]
#                     [--port 8266] [--speed 1]
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
make -s -C "$here"
exec python3 -I "$here/gui/server.py" "$@"
