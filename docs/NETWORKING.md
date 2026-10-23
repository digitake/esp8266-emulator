# Virtual networking

An optional libslirp backend and Arduino core 3.1.2/lwIP2 adapter connect guest
Ethernet frames to the host. This is an experimental virtual station: the guest
still runs its own TCP/IP and application protocols, while the adapter supplies
station state and Wi-Fi events. It does not emulate radio association or RF.

The default build stays offline. Build the network runner on Linux or WSL with
the `libslirp-dev`, `libglib2.0-dev` and `pkg-config` packages installed:

```sh
make network
make check-network-backend
python3 -I scripts/network_profile.py --core-version 3.1.2 \
  --image /path/to/current.bin /path/to/current.elf \
  --image /path/to/update.bin /path/to/update.elf \
  --out /tmp/network-profile.txt
build/esp8266emu-net --flash /path/to/current.bin \
  --net-profile /tmp/network-profile.txt --speed 1 --max-seconds 30 \
  --no-stdin --report /tmp/network-report.json
```

Every image needs its matching unstripped ELF. The profile generator checks
executable bytes and Arduino CRC fields before using its symbols. The runner
matches the actual application bytes after eboot and binds that image's hooks.
An unknown update image stops execution instead of reusing old addresses.
Persistent configuration beyond the BIN is excluded from image matching.
Profiles use version 2 and verify the source BIN's SHA256 before binding its
symbols. Rebuild a BIN after generating a profile and the runner refuses it;
regenerate the profile with its matching ELF. Old version 1 profiles must also
be regenerated.

Arduino's raw Updater may preserve the running flash mode by changing header
byte 2. The adapter permits only that one byte to differ, with a valid mode
0–3; every other byte must match. It reports this normalization and rejects
ambiguous profiles. This does not model the electrical behavior of SPI modes.

Live networking requires positive speed; start with `--speed 1`. The backend
uses host time while CPU and peripheral timers use guest time. Live network
runs are separate from the exact deterministic offline regression checks.

The guest NAT subnet is `10.0.2.0/24`, with host access at `10.0.2.2`, DNS at
`10.0.2.3` and the first DHCP lease at `10.0.2.15`. To reach a station-mode guest
web server, add `--net-forward 8080:80` and open `http://127.0.0.1:8080/`.
Forwarded ports bind to loopback. `--net-capture /tmp/network.pcap` records raw
Ethernet packets for inspection; captures may contain application credentials.

Use `--net-host broker.example.test=10.0.2.2` to map a test hostname to a local
service. The guest still sends a real DNS request to `10.0.2.3`; the backend
answers matching IPv4 A queries. Other names use the normal DNS backend.
Repeat the option for up to 16 names. Overrides remain in place across resets.

The adapter currently models one station interface. Real guest DHCP, DNS, MQTT,
HTTP update and station web upload have passed with the bundled reference.
WiFi OFF/STA transitions and DHCP reacquisition are checked on both OTA boots.
SoftAP recovery, RF/WPA behavior, sleep transitions and arbitrary SDK versions
are not established.
Blocking network operations inside injected Wi-Fi event callbacks are not
supported; the live reference processes MQTT in the application loop.

For firmware built with Arduino core 3.1.2 and lwIP2, the implemented boundary is
the glue between lwIP and the closed SDK. The adapter hooks Ethernet output before
`eagle_lwip_if` / `ieee80211_output_pbuf`, and inject receive frames through the
paired glue entry points. The real guest stack, sockets and application
continue executing from the unmodified image.

## Guest interface

The core declares the boundary in `tools/sdk/lwip2/include/glue.h`; its
implementations are in `builder/glue-lwip/lwip-git.c` and
`builder/glue-esp/lwip-esp.c`. These are external core sources, not files bundled
with this emulator.

| Boundary | Adapter responsibility |
| --- | --- |
| `glue2esp_linkoutput` | Validate and copy one Ethernet frame to a bounded host queue; return an error or success |
| `esp2glue_pbuf_freed` | Release each accepted TX reference exactly once after host consumption |
| `esp2glue_alloc_for_recv` | Allocate a guest receive buffer and obtain its pbuf/payload pointers |
| `esp2glue_ethernet_input` | Transfer a received frame through the guest's normal ownership path |
| `esp2glue_netif_update` | Initialize the lwIP interface from a valid SDK peer |
| `esp2glue_netif_set_up1down0` / `esp2glue_dhcp_start` | Bring the link up and let real lwIP request a lease |

Resolve addresses from each matching ELF and record image hashes. Do not reuse
addresses from another build, rewrite guest instructions or assume that a
similarly named SDK has the same ABI. Decline networking when the required
boundary cannot be identified reliably.

## Ownership and scheduler work

The output glue may flatten a pbuf chain or take an extra reference before
calling the SDK boundary. An accepted frame therefore needs exactly one deferred
release; a rejected frame must receive no completion callback. On RX, handle
allocation failure before touching the payload pointer and transfer ownership
according to the matching glue implementation. Test queue overflow, retransmission,
reset with outstanding work and allocation failures for leaks or double frees.

Run guest callbacks at a safe scheduler point outside active interrupts and
recursive output calls. Associate queued work with a boot generation and discard
stale work after reset. Validate all guest pointers and lengths. The
`guest_call` helper passes six register arguments and places remaining words
in a saved, aligned CALL0 stack area, including the seventh netif argument.

## Association and host backend

Assigning an IP is insufficient. Establish the SDK peer netif first, provide
coherent SSID/BSSID/channel and synthetic RSSI, deliver connected/got-IP events,
and handle disconnect/reconnect queries. Applications may check both connection
state and saved configuration. These transitions need verification for each SDK.

The optional libslirp user-mode NAT backend supplies DHCP and DNS while the guest
runs its own protocols. TAP is not implemented. Keep the GUI loopback-only and
use local test services for repeatable integration checks. The GUI's opt-in
`--net-profile` starts this runner only for images whose SHA-256 the profile
lists, keeps its own loopback bind and Host/Origin checks, and shows the runner's
`net` events (binding, station state, address and packet counts) in its Network card.

This approach would not test WPA authentication/encryption, RF contention,
acknowledgments/retries, real AP interoperability, roaming, radio sleep or measured
RSSI. Keep scans empty or label synthetic scan fixtures explicitly. Host I/O adds
asynchronous timing; retain deterministic offline tests separately.

## Firmware acceptance checks

Compatibility must be demonstrated for each identified SDK/image. Backend and
queue unit tests alone do not establish firmware compatibility. The required
integration checks are:

1. Establish the guest interface and obtain DHCP/DNS through its actual stack.
   Record packet traces and masked application output.
2. Connect the guest application's MQTT client to a local broker and record a
   CONNECT/CONNACK plus publish/subscribe round trip.
3. Fetch an update from a local HTTP server using the application's accepted OTA
   path. Observe actual Updater writes and RTC command creation, then eboot copy
   and boot of the new image. Compare raw/gzip results to rehearsal invariants.

The current `scripts/ota_rehearsal.py` begins after staging by injecting bytes
and the RTC command. Its copy results establish none of the network or live
Updater steps above. See [testing](TESTING.md) for its precise scope.
