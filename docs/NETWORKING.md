# Network feasibility

Host networking is a design proposal, not an implemented feature. The current
MAC model completes selected no-ACK transmissions and exposes a virtual timer.
It provides neither station association nor a host Ethernet interface.

For firmware built with Arduino core 3.1.2 and lwIP2, a plausible boundary is
the glue between lwIP and the closed SDK. Hook Ethernet output before
`eagle_lwip_if` / `ieee80211_output_pbuf`, and inject receive frames through the
paired glue entry points. The real guest stack, sockets and application would
continue executing from the unmodified image.

## Candidate interface

The core declares the boundary in `tools/sdk/lwip2/include/glue.h`; its
implementations are in `builder/glue-lwip/lwip-git.c` and
`builder/glue-esp/lwip-esp.c`. These are external core sources, not files bundled
with this emulator.

| Boundary | Prototype responsibility |
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
stale work after reset. Validate all guest pointers and lengths. The current
`guest_call` helper passes six register arguments; a direct call to the
seven-argument `esp2glue_netif_update` needs CALL0 stack-argument support or an
existing SDK caller that supplies the complete ABI.

## Association and host backend

Assigning an IP is insufficient. Establish the SDK peer netif first, provide
coherent SSID/BSSID/channel and synthetic RSSI, deliver connected/got-IP events,
and handle disconnect/reconnect queries. Applications may check both connection
state and saved configuration. These transitions need verification for each SDK.

A user-mode NAT backend is a practical first candidate: it can expose an isolated
Ethernet segment with DHCP and DNS while the guest runs its own protocols. TAP
requires host network configuration and privileges. Backend dependencies and
packaging remain undecided. Keep the GUI loopback-only and use local test services.

This approach would not test WPA authentication/encryption, RF contention,
acknowledgments/retries, real AP interoperability, roaming, radio sleep or measured
RSSI. Keep scans empty or label synthetic scan fixtures explicitly. Host I/O adds
asynchronous timing; retain deterministic offline tests separately.

## Prototype acceptance

A first prototype for one identified SDK/image is a multi-day task; a rough
2–4 day estimate covers the Ethernet backend, ownership, association transitions
and MQTT/OTA validation. General support across SDKs needs additional work.

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
