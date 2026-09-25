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
  -DPICO_SDK_PATH=/absolute/path/to/pico-sdk \
  -DRABBIT_LED_PIN=<verified-led-pin> \
  -DRABBIT_AUDIO_PIN=<verified-audio-pin>
cmake --build build/rp2350
```

The generated UF2 appears under `build/rp2350`. Do not flash it before verifying
the board revision, power circuitry, serial reset behavior, and external wiring.
The Pico SDK's generated PIO header is compiled as C++20; the portable engine
and RP2350 application sources remain C++17.

## RP2350 USB diagnostics and UART service

The RP2350 image exposes a USB CDC console after boot. Connect at `115200` baud
and expect `Rabbit RP2350 diagnostic firmware ready`. The bounded commands are
`help`, `status`, `led`, `audio`, and `off`. `led`, `audio`, and local stop
intentionally report unavailable until their physical pins are verified and
supplied at build time. The same image runs the bounded UART1 service at
115200 baud using the verified onboard RP2350 pins 28/29/27/26. It accepts a
session handshake plus `PrepareBegin`, `Start`, `Stop`, and `Status` frames;
COBS framing, CRC, request IDs, and session IDs are enforced. The initial
prepare payload is 13 bytes: flags, followed by big-endian `distance_mm`,
`target_ms`, and `interval_ms` fields. It represents one whole-pool swim on
the currently conservative 25-yard controller profile.

## ESP32-C6 build

Install and source the exact ESP-IDF release in `toolchains.lock`, then build
in this directory:

```sh
cd firmware/esp32
idf.py set-target esp32c6
idf.py build
```

The ESP application configures the verified interprocessor UART pins (TX 7,
RX 6, RTS 4, CTS 5), establishes/re-establishes a session, and forwards the
bounded vertical slice through `POST /api/workout/prepare`,
`POST /api/workout/start`, `POST /api/workout/stop` (and legacy `POST /stop`),
and `GET /api/set-status`. Prepare accepts an optional JSON object with
`distanceMm`, `targetMs`, `intervalMs`, `audio`, `waitForInterval`, and
`direction: "far-to-near"`; omitted numeric values select a 25-yard,
30-second test swim. A transport failure returns HTTP 503, never a false
success.

Wi-Fi lifecycle, static browser assets, complete DeckScript/plan decoding,
protected configuration persistence, and hardware qualification still remain
before this can replace MicroPython. See `../docs/cpp-migration-plan.md` for
the acceptance gates and recovery rules.
