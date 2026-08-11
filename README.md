# Pump Bug firmware

Pump Bug turns an M5StickS3 into an espresso extraction monitor. Attached
magnetically to the exterior of the machine, it detects pump vibration with its
built-in accelerometer and combines that signal with weight readings from a
supported Bluetooth scale. It shows live yield and flow, alerts the operator
near a target yield, stores shot history, and provides both an on-device
interface and a browser interface over Wi-Fi.

## Start here

- Read [the user guide](docs/user-guide.md) to install Pump Bug and use its
  extraction, target alert, scale, history, and power features.
- Read [the web interface guide](docs/web-guide.md) to connect and pair a
  browser, follow live extractions, manage history, and use diagnostic logs.
- Open [the visual system overview](docs/overview.html) for the major components
  and the data that passes between them.
- Read [`src/main.cpp`](src/main.cpp) for boot and runtime composition.
- Read [`src/apps/extraction/ExtractionRecorder.h`](src/apps/extraction/ExtractionRecorder.h)
  for the extraction state machine.
- Run the host tests to exercise the platform-independent behavior.

## Hardware and software

- M5Stack M5StickS3
- Built-in BMI270 accelerometer
- Acaia scale using the version 2 Bluetooth protocol
- PlatformIO with the Arduino framework for ESP32
- CMake and a C++17 compiler for host tests
- Node.js for the JavaScript parity test

The PlatformIO configuration pins the platform and library versions used by the
firmware.

## Build

Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/index.html),
then run:

```sh
pio run -e release
```

The build regenerates the browser bundle and embeds its compressed assets in the
firmware. The result is `.pio/build/release/firmware.bin`.

The release version is read from [`VERSION`](VERSION), compiled into the boot
log, and shown on the browser interface home page.

Connect the device over USB to upload and monitor it:

```sh
pio run -e release -t upload
pio device monitor
```

The firmware uses a single 3.5 MiB application slot and a 4.375 MiB shot-history
partition; it does not reserve space for OTA updates. Installing it over
firmware with a different partition layout requires a full-chip erase. Back up
any shots you want to keep, then run:

```sh
pio run -e release -t erase
pio run -e release -t upload
```

The first boot prepares the empty history partition automatically.

Use `pio run -e debug` for an unoptimized build with debug symbols, INFO-level
ESP32 logging, and serial UI automation. The button, orientation, and screenshot
protocol is documented in
[`src/ui/UiDebugRemote.h`](src/ui/UiDebugRemote.h).

## Test

The host suite requires a C++17 compiler and Node.js:

```sh
cmake -S . -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

The suite covers the scale protocol, vibration trigger, extraction recorder,
flow analysis, target alert, wire format, power policies, and C++/JavaScript
parity for the robust-flow implementation.

Hardware-dependent behavior still requires an M5StickS3 and scale. In
particular, verify display rendering, Bluetooth and Wi-Fi coexistence, sleep and
wake behavior, and audio after changes to those areas.

## Repository map

| Path | Responsibility |
|---|---|
| `src/apps/extraction/` | Shot recording, classification, target alert, history, and UI |
| `src/apps/backflush/` | Vibration-driven backflush timer  |
| `src/vibration/` | Accelerometer processing and pump-window detection |
| `src/ble/` | Acaia protocol, scale connection, and Bluetooth radio management |
| `src/net/` | Wi-Fi, authentication, HTTP, and server-sent events |
| `src/power/` | Sleep, battery, scale-radio, and speaker power policy |
| `src/ui/` | Display, navigation, layout, theme, and reusable controls |
| `web-src/` | Browser interface source |
| `test/` | Host tests and their reference data |
| `scripts/` | Versioning, browser-asset, license, and release-packaging tools |
| `packaging/m5burner/` | M5Burner package metadata and packaging documentation |
| `VERSION` | Canonical release version consumed by builds and packaging |

## How the firmware works

### Detecting when the pump runs

The device uses its built-in accelerometer to detect the vibrations produced by
the machine's pump. The vibration code looks for the pump's steady rhythm and
reports when it starts and stops. Mechanical disturbances and empty flushes can
also produce vibration without an extraction, so pump detection only determines
when the recorder should consider scale readings.

Start with [`src/vibration/VibrationSensor.cpp`](src/vibration/VibrationSensor.cpp)
and
[`src/vibration/VibrationWindowTrigger.h`](src/vibration/VibrationWindowTrigger.h).

### Reading the scale

The Bluetooth service finds a supported scale, connects to it, and decodes its
weight, timer, and battery messages. The rest of the firmware sees one current
scale reading instead of dealing with Bluetooth callbacks or packet formats. If
the connection is lost, the service searches and reconnects.

Start with [`src/ble/BleScaleService.cpp`](src/ble/BleScaleService.cpp) and
[`src/ble/AcaiaV2Codec.cpp`](src/ble/AcaiaV2Codec.cpp).

### Recording an extraction

When the pump starts, the recorder begins collecting scale readings. It keeps
the record open briefly after the pump stops because coffee may still be
falling into the cup, and it can join a short pump restart to the same
extraction. From the stored weights it calculates yield and flow. A record is
saved as a shot only when enough weight accumulated for long enough; flushes,
scale knocks, and other short events are left out of history.

The stored weights remain unchanged. The live display, the final shot decision,
and the target alert each use those readings for their own purpose, so a
temporary bad reading does not alter the stored record.

Start with
[`src/apps/extraction/ExtractionRecorder.h`](src/apps/extraction/ExtractionRecorder.h)
and
[`src/apps/extraction/ExtractionController.cpp`](src/apps/extraction/ExtractionController.cpp).

### Showing the shot on the device

The device screen shows live weight, yield, flow, and the target alert. After an
extraction it can show the saved summary and chart, or replay the last shot from
its recorded scale readings. Setup and diagnostic screens use the same
two-button navigation and adapt to the screen orientation.

Start with [`src/apps/extraction/ui/`](src/apps/extraction/ui/) and
[`src/MainNavigation.h`](src/MainNavigation.h).

### Saving shot history

The flash partition retains up to
[`ShotStore::kMaxShots`](src/apps/extraction/history/ShotStore.h) accepted shots
(currently 250). After a new shot has been written and verified, the oldest
records are removed when necessary to stay within that limit. A failed write
therefore does not discard the existing history.

Each shot uses the same versioned, compact record format in flash and when sent
to a browser. This includes the pump events and scale readings needed to draw
the chart, recalculate flow, download the data, or replay the shot on the
device.

Start with
[`src/apps/extraction/history/ShotStore.h`](src/apps/extraction/history/ShotStore.h)
and [`src/apps/extraction/ExtractionWire.h`](src/apps/extraction/ExtractionWire.h).

### Connecting to Wi-Fi

Pump Bug works without Wi-Fi and leaves the Wi-Fi radio off until it has been
configured. The on-device setup wizard starts a temporary network and guides a
browser through selecting the home network. Once setup completes, the saved
configuration can reconnect automatically on later boots. Wi-Fi can also be
turned off without deleting the saved network.

Start with [`src/net/WifiManager.cpp`](src/net/WifiManager.cpp) and
[`src/apps/wifi/WifiSetupWizard.cpp`](src/apps/wifi/WifiSetupWizard.cpp).

### Serving the browser interface

The browser interface is built into the firmware and served directly by Pump
Bug over the local network; it does not depend on an internet service. It adds
configuration, a larger live chart, saved-shot history, downloads, and
diagnostic views. Live updates use a separate event stream so a long-running
browser connection does not block ordinary web requests.

The browser repeats part of the flow calculation for charting. The host tests
compare its JavaScript result with the firmware result using the same reference
data.

Start with [`src/net/HttpServer.cpp`](src/net/HttpServer.cpp),
[`src/apps/extraction/ExtractionStream.cpp`](src/apps/extraction/ExtractionStream.cpp),
and [`web-src/`](web-src/).

### Authorizing browsers

Device settings, live data, and shot history are available only to paired
browsers. The device opens a temporary pairing window and shows either a QR code
or a four-digit PIN, proving that the person pairing can see the physical
screen. A successful pairing stores a random credential in the browser and the
matching authorization on the device.

A limited crash summary remains available without pairing so a failure can be
diagnosed when authentication is unavailable. It contains the failing task,
processor exception details, and backtrace addresses, but no settings, live
data, or shot records.

Pairing survives ordinary reboots. The Wi-Fi screen can unpair all browsers,
and resetting the network configuration also removes every paired credential.

Start with [`src/net/Auth.h`](src/net/Auth.h) and
[`src/net/Auth.cpp`](src/net/Auth.cpp).

### Managing power

One power manager coordinates the display, processor, status light, Bluetooth,
speaker amplifier, and shutdown behavior. After a period without activity, the
display dims and the processor speed is reduced to save energy. On battery power
the device eventually switches off; while connected to external power it
remains on.

Bluetooth remains available while the display is dimmed, so a scale can connect
without first waking the device. Other hardware is controlled separately: the
speaker amplifier is powered only when a sound needs it, the unused external
5 V output stays off, and Wi-Fi follows its own configuration and connection
state.

If the battery remains critically low, the device warns the user and then
shuts down before the voltage becomes unreliable. Before switching off it
records the reason, tells connected browsers, and configures the motion sensor
so moving the machine can wake it again.

Start with [`src/power/PowerManager.cpp`](src/power/PowerManager.cpp),
[`src/power/PowerSavingPolicy.h`](src/power/PowerSavingPolicy.h), and the
corresponding [`test/test_power_manager.cpp`](test/test_power_manager.cpp) and
[`test/test_low_battery_shutdown_policy.cpp`](test/test_low_battery_shutdown_policy.cpp)
host tests.

## License

Copyright (C) 2026 ludable.

Pump Bug firmware is distributed under the
[GNU Affero General Public License version 3](LICENSE). See
[`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md) for source provenance and
third-party notices.
