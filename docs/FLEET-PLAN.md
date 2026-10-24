# Fleet plan: ten devices and a 3D view

Status: **proposal, nothing built yet.** This plan describes how to run up to ten
emulated devices at once, each with its real firmware, and show them in a
three.js house in the browser. The devices talk only through an MQTT broker; there
is no direct device-to-device traffic.

## Goal

- Up to 10 devices, each running its unmodified firmware image on the virtual
  network ([virtual networking](NETWORKING.md)).
- All communication through an MQTT broker (TLS on port 8883) and an OTA web
  server on the host.
- A 3D house in the browser: each device shows its relay and LED state live, and
  clicking it presses its button.
- Scripted scenarios across the whole fleet, such as a button press that must reach
  MQTT, or an OTA update of every device.

## Capacity

The emulator interprets every guest instruction, about 80 million a second for one
device in real time. Arduino firmware that polls in `loop()` never idles, so the
cost does not drop when nothing happens. Measured with a smart-switch firmware on
an 8-core / 16-thread laptop (Ryzen 7 5800H):

| Devices at once | Speed against real time | CPU per device | RAM per device |
| --- | --- | --- | --- |
| 1 | 1.00× | 0.5 core | 4.6 MB |
| 4 | 0.97× | 0.8 core | 4.6 MB |
| 8 | 0.74× | 1.0 core | 4.6 MB |
| 12 | 0.52× | 1.0 core | 4.6 MB |

Memory is not a constraint; CPU is. Shared cores and lower all-core clocks make each
device more expensive as more run.

- **Laptop:** 4–5 devices in real time; 10 devices run at about 0.6×. That is
  enough to build and check behavior, but MQTT brokers drop a client that is silent
  for 1.5× its keepalive. At 0.6× the guest's keepalive arrives about 1.7× late by
  the broker's clock, so full ten-device runs need the bigger machine, or a test
  broker that tolerates late keepalives.
- **Bigger machine:** 12 or more physical cores, or a cloud VM with about 24 vCPUs,
  runs all ten in real time. Nothing else in the plan changes.

Profiles of polling firmware show time spread across `millis()` and many small
functions, with no single hotspot. The emulator deliberately never replaces
firmware functions, so there is no cheap speed-up to rely on.

## Architecture

```text
Browser: three.js house  ──  one live event stream (SSE) + commands (HTTP POST)
          │
fleet controller (Python standard library, 127.0.0.1)  ── fleet.json
          │  stdin commands / stdout JSON events, one pair per device
N × build/esp8266emu-net   (own MAC, own saved flash, own web port, --net-local)
          │  every host name resolves to the host, 10.0.2.2
MQTT broker (TLS :8883) and OTA web server, on the host
```

The controller is Python, like the GUI server. It only moves events and commands:
a few kilobytes a second per device, against about half a core per device for the
emulator itself. It reuses the GUI server's credential masking, upload checks,
network-profile checks and loopback-only Host/Origin guards. Server-sent events and
HTTP POST are what a browser page needs; a WebSocket endpoint can follow if a game
engine such as Unity or Godot is added.

## Steps

1. **Unique device identity.** Every emulated chip now reports the same MAC
   (`18:FE:34:EE:B0:01`), set in the eFuse words in `src/soc.c`. Firmware that
   derives its device ID or MQTT client ID from the MAC would make ten devices take
   over each other's broker session. Add `--mac` to the runner, encoded into the
   eFuse words the SDK reads, so the station MAC, soft-AP MAC and chip ID all
   change. Done when two devices with different MACs keep separate MQTT sessions on
   one broker.
2. **Per-device settings.** Firmware that keeps its Wi-Fi and broker settings in
   flash, and is normally provisioned over its own soft AP, needs them written in
   advance, because the virtual network has no soft AP. A tool in the firmware's own
   repository writes each device's settings (Wi-Fi, broker, credentials, device ID,
   topic root) into a `.flash` file. The GUI already loads such a file next to the
   `.bin`, and the controller will too.
3. **Fleet controller** (`fleet/`): reads `fleet.json`, which lists each device's
   name, `.bin`, `.elf`, `.flash`, MAC, GPIO profile and position in the house. It
   starts every device on the virtual network with `--net-local`, gives each its own
   work directory and host port for its web server, keeps each device's flash
   between runs, and restarts a device that exits.
   - API: `GET /api/fleet`, one event stream for all devices or one (`/events`,
     `/events?device=NAME`), and `POST /api/cmd` with a device name and the
     existing commands: pin drive, button press, `rx`, reset, power cycle.
   - Health: each device's speed against real time, with a warning when one falls
     behind.
4. **Overview page:** one row per device with boot stage, Wi-Fi and network state,
   the last console lines and a link to its web server.
5. **3D house (three.js):** rooms from a layout file; each device placed from
   `fleet.json`. Lamps and LEDs follow the device's pins live, and clicking a switch
   presses its button. three.js is vendored into the repository under its MIT
   license, because the pages must work offline and load nothing from a CDN.
6. **Scenarios:** command-line scripts that drive the fleet and check MQTT, such as
   "press switch 3, expect its state topic within 2 s", or an OTA of every device
   with each one's new version on the broker. They run unattended, so the bigger
   machine can run them in CI.

## Development order

| Phase | Machine | Delivers |
| --- | --- | --- |
| 1 | Laptop | `--mac`, the per-device settings tool, the controller with two devices, the overview page |
| 2 | Laptop | Four or five devices in real time against a local TLS broker; fleet OTA |
| 3 | Laptop | The three.js house |
| 4 | Bigger machine | Ten devices in real time; scenarios in CI |

## Open points

- The MQTT broker is the user's: Mosquitto with a TLS listener on 8883 and a
  self-signed certificate is enough for firmware that does not verify the
  certificate.
- Devices are not time-synchronized with each other, as on real hardware.
- No device-to-device networking. Each device has its own NAT network; adding a
  shared virtual segment is a separate project.
- The per-device settings tool is specific to each firmware and stays out of this
  repository.
