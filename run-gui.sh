#!/usr/bin/env bash
# ESP8266 emulator GUI on http://127.0.0.1:8266/ (loopback only). WSL/Linux: needs gcc, make, python3.
# Usage: ./run-gui.sh [--bin firmware.bin] [--elf firmware.elf] [--profile-file examples/gpio-profile.json]
#                     [--port 8266] [--speed 1] [--workdir DIR]
#                     [--offline | --internet] [--net-forward 8080:80] [--net-host NAME=IP] [--net-capture FILE]
#                     [--net-profile profile.txt]
# Drop a firmware.bin with its .elf on the page. When build/esp8266emu-net builds (libslirp-dev, libglib2.0-dev and
# pkg-config), an Arduino 3.1.2 image dropped with its .elf gets the experimental virtual network: local only, every
# name leads to this host, unless --internet. --offline turns it off; --net-profile runs only the images it binds.
set -euo pipefail
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
make -s -C "$here"
exec python3 -I "$here/gui/server.py" "$@"
