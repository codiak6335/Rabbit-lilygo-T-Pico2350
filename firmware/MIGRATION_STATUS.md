# C++ migration status

Last updated: 2026-09-25

- [x] Portable, fixed-capacity C++17 domain model, pool geometry, and workout
  state machine with nonblocking audio scheduling.
- [x] Shared COBS/CRC UART framing and transactional, checksummed plan buffer.
- [x] Native simulator, fake-clock coverage, protocol failure coverage, and
  AddressSanitizer/UndefinedBehaviorSanitizer host test configuration.
- [x] RP2350 Pico SDK project cross-compiles; its bounded UART0 command service
  handles session handshake, 1–256-rep pace sets, continuous sprint sets,
  Start, Stop, Cancel, and Status.
- [x] ESP32-C6 ESP-IDF project cross-compiles and forwards pace/sprint HTTP
  commands to the RP2350, returning 503 on transport failure.
- [x] Toolchain versions are pinned in `toolchains.lock`; host, RP2350, and
  ESP32-C6 use separate build directories.
- [x] Board profiles target the separate Waveshare RP2350B Plus W and
  ESP32-C6-LCD-1.9 connected on GP0/GP1 ↔ GPIO17/GPIO16.
- [x] Both C++ builds flashed to the separate boards; ESP boot-time UART
  status probe and RP request/response counters pass on 2026-09-25.
- [x] ESP32-C6 joins a provisioned Wi-Fi SSID, reports its DHCP address over
  USB, and renders connection/IP status through the onboard LCD driver. The
  `beaver` connection and HTTP `/` and `/api/set-status` returned successfully
  on 2026-09-25. The user confirmed the LCD displays the IP after correcting
  GPIO15's active-low backlight polarity.
- [x] Temporary AP-only firmware flashed on 2026-09-25. USB boot log confirms
  `Rabbit-Test-6905`, DHCP at `192.168.4.1`, and no station connection;
  Beaver credentials remain stored. The user confirmed the LCD shows
  `192.168.4.1` and the direct-AP webpage loads on the phone.
- [x] Station/AP test firmware now joins `beaver` from stored NVS credentials,
  displays its DHCP address on the LCD, and retains the Rabbit test AP as a
  fallback. On 2026-09-25 it received `192.168.0.178`; host requests returned
  HTTP 200 for the Coach On Deck page, assets, pool/sets data, and RP status.
- [x] Checked-in Coach On Deck HTML/CSS/JavaScript, pool list, and prompt are
  embedded in the ESP app. Saved sets are validated and stored in ESP NVS.
  Both updated boards are flashed; USB confirms the AP HTTP server and UART
  handshake. Phone browser verification of the new UI is pending.
- [x] Native debug and sanitizer tests pass for repeat, continuous,
  Stop/Continue, Cancel, and the expanded status frame.
- [x] DeckScript 2 compiled-plan JSON parsing, bounded CRC-checked UART plan
  transfer, and Pico swim/rest/activity execution are flashed on both boards.
  Native and sanitizer tests cover all checked-in DeckScript examples, surge,
  negative split, mixed steps, invalid input, and transfer sequencing. A live
  short surge/rest/swim workout completed through HTTP and UART, and a 30-step
  plan prepared through multi-chunk transfer on 2026-09-25. Phone-browser and
  pool-side acceptance remain pending.
- [x] RP2350 now supplies authoritative set details in its status frame. The
  ESP webpage retained `rep 3 of 20` and `200 yd` through an ESP restart during
  a running paired-device test on 2026-09-25. The LCD driver and polling task
  started without errors; physical confirmation of the new workout screen is
  still pending.
- [ ] Verify external LED and audio pin assignments on the RP2350 and visually
  confirm the LCD's workout phase, rep count, countdown, and IP display.
- [ ] Qualify the paired RP2350/ESP32 UART service, including real Stop
  latency, reboot reconciliation, and corrupted-frame recovery.
- [ ] Finish ESP Wi-Fi lifecycle, protected pool/Wi-Fi editing, calibrated
  LED geometry, and remaining full-plan edge-case acceptance. Unsupported
  plans/settings must continue to return explicit errors.
- [ ] Run paired-device, browser compatibility, timing, long-duration, power
  loss, recovery, and physical-pool acceptance tests.
- [ ] Remove the MicroPython production runtime only after the preceding
  hardware acceptance gates pass. The Python implementation remains the
  current reference and recovery path.
