#!/bin/bash
# ESP8266 emulator on macOS: double-click in Finder, or run from Terminal. Builds the emulator, starts the GUI and
# opens http://127.0.0.1:8266/ in your browser; drop a firmware.bin with its .elf on the page. Ctrl+C or closing the
# Terminal window stops it. Options go to run-gui.sh, for example:  ./emulator.command --port 8276 --offline
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
port=8266
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    [ "${args[i]}" = "--port" ] && port="${args[i + 1]:-8266}"
done
url="http://127.0.0.1:$port/"

running() { curl -fs -m 2 "${url}api/state" >/dev/null 2>&1; }
if running; then
    echo "The emulator GUI is already running at $url"
    open "$url"
    exit 0
fi

# open the browser once the server answers
( for _ in $(seq 240); do running && { open "$url"; exit 0; }; sleep 0.5; done ) &
opener=$!
trap 'kill $opener 2>/dev/null' EXIT

bash "$here/run-gui.sh" "$@"
status=$?
if [ $status -ne 0 ] && [ $status -ne 130 ]; then
    echo
    read -r -p "The emulator stopped with an error (exit $status). Press Return to close this window."
fi
exit $status
