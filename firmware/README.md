# Native firmware

This directory contains the in-progress production replacement for the
MicroPython controller. The existing Python implementation remains the
behavioral reference until hardware qualification is complete.

## Layout

- `core/`: allocation-free-at-run-time workout validation, pool geometry, and
  scheduler. It has no SDK, file, socket, or sleep dependency.
- `protocol/`: bounded COBS/CRC UART frames and transactional plan-transfer
  buffer shared by the two processors.
- `host/`: desktop executable using the same portable core.
- `rp2350/`: Pico SDK application, local-stop loop, and WS2812 PIO/DMA output.
- `esp32/`: ESP-IDF application boundary for the C6 HTTP/network process.

## Build the portable code

Until CMake is installed, the host checks can run directly with the compiler:

```sh
mkdir -p /tmp/rabbit-native-build
c++ -std=c++17 -Wall -Wextra -Wpedantic -Wconversion -Wshadow \
  -Ifirmware/core/include -Ifirmware/protocol/include \
  firmware/core/src/pool_geometry.cpp firmware/core/src/workout_engine.cpp \
  firmware/protocol/src/framing.cpp firmware/protocol/src/workout_service.cpp \
  tests/native/test_main.cpp \
  -o /tmp/rabbit-native-build/rabbit_native_tests
/tmp/rabbit-native-build/rabbit_native_tests
```

With CMake, build the same targets from the repository root using the supplied
host presets:

```sh
cmake --preset host-debug
cmake --build --preset host-debug
ctest --test-dir build/host-debug --output-on-failure
```

Run the sanitizer configuration separately:

```sh
cmake --preset host-sanitize
cmake --build --preset host-sanitize
ctest --test-dir build/host-sanitize --output-on-failure
```

## RP2350 build

Use the exact Pico SDK and ARM toolchain listed in `toolchains.lock`. The external Rabbit
LED and audio wiring is unverified, so both are disabled by default. Supplying a
pin is a board-qualification step, not a default production configuration.

```sh
cmake -S firmware/rp2350 -B build/rp2350 -G Ninja \
  -DPICO_SDK_PATH=/absolute/path/to/pico-sdk
cmake --build build/rp2350
```

The project defaults to the Waveshare RP2350B Plus W board profile in
`rp2350/boards/`. The generated UF2 appears under `build/rp2350`.
External LED, audio, and local-stop pins remain disabled until their wiring is
verified.
The Pico SDK's generated PIO header is compiled as C++20; the portable engine
and RP2350 application sources remain C++17.

## RP2350 USB diagnostics and UART service

The RP2350 image exposes a USB CDC console after boot. Connect at `115200` baud
and expect `Rabbit RP2350 diagnostic firmware ready`. The bounded commands are
`help`, `status`, `led`, `audio`, and `off`. `led`, `audio`, and local stop
intentionally report unavailable until their physical pins are verified and
supplied at build time. The same image runs the bounded UART0 service at
115200 baud. It accepts a
session handshake plus `PrepareBegin`, `Start`, `Stop`, `Cancel`, and `Status` frames;
COBS framing, CRC, request IDs, and session IDs are enforced. The RP2350 uses
UART0 on physical header pins 1/2 (GP0 TX/GP1 RX), connected to ESP32-C6
GPIO17 RX/GPIO16 TX, with a shared ground on RP2350 header pin 3. No hardware
flow control is wired. The initial
prepare payload is 13 bytes: flags, followed by big-endian `distance_mm`,
`target_ms`, and `interval_ms` fields. The 21-byte set payload adds big-endian
repetitions, final-rep target, and surge percentage. This supports 1–256
whole-pool pace repetitions and a continuous sprint loop. A compiled DeckScript
plan uses checksummed `PrepareBegin`/`PrepareChunk`/`PrepareCommit` frames and a
bounded 12 KiB binary plan. The Pico validates and runs swim, rest, and activity
steps, including continuous loops, per-rep progression, surge, and negative
splits. The base 28-byte UART status frame includes remaining milliseconds,
cycle, set count, current distance and target, and a continuous-mode flag;
DeckScript status appends the current step, strategy, swim count, and bounded
name/label text. External LED pin assignment and calibrated physical geometry
are still unverified.

## ESP32-C6 build

Install and source the exact ESP-IDF release in `toolchains.lock`, then build
in this directory:

```sh
cd firmware/esp32
idf.py set-target esp32c6
idf.py build
```

The ESP application configures UART0 on TX GPIO16/RX GPIO17 without hardware
flow control. Its console uses USB Serial/JTAG so application logs do not enter
the interprocessor link. At startup it probes RP2350 status and reports the
result on its USB console. The onboard 170×320 ST7789V2 LCD displays the live
workout phase, distance, rep count, countdown, and IP address. The ESP polls
RP2350 status for this display and the webpage's set details, so an ESP restart
does not reset the visible rep total. It establishes/re-establishes a session and forwards the
bounded vertical slice through `POST /api/workout/prepare`,
`POST /api/workout/start`, `POST /api/workout/stop`, the legacy quick-set
`GET /prep`, `GET /start`, `GET /startsprint`, `GET /stop`, `GET /cancel-prep`,
and `GET /api/set-status`. The modern prepare route accepts an optional JSON object with
`distanceMm`, `targetMs`, `intervalMs`, `audio`, `waitForInterval`, and
`direction: "far-to-near"`; omitted numeric values select a 25-yard,
30-second test swim. A transport failure returns HTTP 503, never a false
success.

The checked-in Coach On Deck page is embedded at `/`; the former manual test
page is at `/test`. CSS, JavaScript, pool definitions, and the DeckScript prompt
are served from the firmware image. Saved sets begin with the checked-in
`db/sets.json` and subsequent browser edits are validated and committed to
ESP NVS. Pool and Wi-Fi editing intentionally return 501; Wi-Fi credentials
are never served by the browser API. Compiled DeckScript 2 plans from the
browser are validated against the embedded pool list and transferred to the
Pico; unsupported or malformed plans return an error rather than a default
swim. The app uses the 1500 KiB single
app partition; upgrading an earlier 1 MiB build requires flashing its new
partition table alongside the app, without erasing NVS.

The ESP currently joins the provisioned `beaver` network while keeping the
`Rabbit-Test-6905` AP available as a fallback at `http://192.168.4.1/`. The
LCD shows the station's DHCP address after connection; on 2026-09-25 this was
`192.168.0.178`, but the address may change. To
provision an existing Wi-Fi profile without putting its password in source or
on the command line, run this from the repository root (requires `pyserial`):

```sh
python3 tools/provision_esp_wifi.py \
  --port /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_CC:BA:97:F2:69:04-if00 \
  --ssid beaver
```

The tool reads `db/wifi.json`, sends the selected credentials over USB, and
stores them in the ESP's `rabbit_net` NVS namespace. The current station/AP
build loads those credentials on boot, updates the LCD when DHCP assigns an
address, and reports the address
as `Rabbit Wi-Fi connected: ...` on USB. The browser UI is then available
at `http://<that-address>/`. Do not log or commit additional credential
copies when provisioning another device.

Full Wi-Fi lifecycle, protected settings, physical LED/audio wiring,
and phone-browser/pool-side DeckScript acceptance still remain
before this can replace MicroPython. See `../docs/cpp-migration-plan.md` for
the acceptance gates and recovery rules.
