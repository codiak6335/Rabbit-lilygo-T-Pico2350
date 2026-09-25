# C++ migration status

Last updated: 2026-09-19

- [x] Portable, fixed-capacity C++17 domain model, pool geometry, and workout
  state machine with nonblocking audio scheduling.
- [x] Shared COBS/CRC UART framing and transactional, checksummed plan buffer.
- [x] Native simulator, fake-clock coverage, protocol failure coverage, and
  AddressSanitizer/UndefinedBehaviorSanitizer host test configuration.
- [x] RP2350 Pico SDK project cross-compiles; it has a bounded UART1 command
  service for session handshake, simple prepare, Start, Stop, and Status.
- [x] ESP32-C6 ESP-IDF project cross-compiles and forwards the simple workout
  HTTP routes to the RP2350 UART service, returning 503 on transport failure.
- [x] Toolchain versions are pinned in `toolchains.lock`; host, RP2350, and
  ESP32-C6 use separate build directories.
- [ ] Verify external LED and audio pin assignments and bring up the display on
  the target board. These connections are not established by vendor board data.
- [ ] Qualify the paired RP2350/ESP32 UART service, including real Stop
  latency, reboot reconciliation, and corrupted-frame recovery.
- [ ] Implement ESP Wi-Fi lifecycle, static browser assets, complete
  DeckScript/plan decoding/routes, and protected configuration persistence.
- [ ] Run paired-device, browser compatibility, timing, long-duration, power
  loss, recovery, and physical-pool acceptance tests.
- [ ] Remove the MicroPython production runtime only after the preceding
  hardware acceptance gates pass. The Python implementation remains the
  current reference and recovery path.
