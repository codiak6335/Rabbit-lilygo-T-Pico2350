# Rabbit controller

The LilyGO T-Pico2350 is being replaced by two separate boards:

- Waveshare RP2350B Plus W, which will run the workout controller.
- Waveshare ESP32-C6-LCD-1.9, which will provide the display and network interface. The connected ESP identifies as an ESP32-C6FH8 with 8 MB flash. USB identifies the chip, but does not distinguish the LCD board variant.

## Inter-board UART wiring

The boards are already connected with three wires. Pin numbers in the left column are **physical header positions** on the RP2350 board, not GPIO numbers.

| RP2350B Plus W | Signal direction | ESP32-C6-LCD-1.9 |
| --- | --- | --- |
| Pin 1, GP0 / UART0 TX | RP2350 → ESP32-C6 | GPIO17 / UART RX |
| Pin 2, GP1 / UART0 RX | ESP32-C6 → RP2350 | GPIO16 / UART TX |
| Pin 3, GND | Common ground | GND |

The existing connection contains TX, RX, and ground; no RTS/CTS wires were specified. See the [Pico-compatible pinout](https://datasheets.raspberrypi.com/pico/Pico-2-Pinout.pdf) and [ESP32-C6-LCD-1.9 documentation](https://docs.waveshare.com/ESP32-C6-LCD-1.9).

## Printable enclosure

The [Rev D resin-window housing](mechanical/resin_housing/README.md) includes PETG body,
lid frame, window retainer and integral PCB standoffs, plus closed and split-wrap TPU strain
relief STLs for 2.3, 3, 3.5 and 4 mm cable jackets. It uses a cast clear resin
window, a silicone cord lid seal and two oversized cable entries for adhesive
sealing. Editable dimensions, print-fit coupons and assembly instructions are
included. The 82 × 128 × 33.1 mm prototype matches the `rev_d_case` 60 × 100 mm
PCB and its offset mounting hole, with the display centered, LED cable entry at
the top and power cable entry at the bottom. The captured KiCad reference is
checked during regeneration. Physical fit and splash/submersion qualification
remain to be performed.

## Current firmware status

The checked-in C++ firmware now targets this three-wire link. The RP2350 uses UART0 on GP0/GP1 without the LilyGO I²C expander startup; the ESP32-C6 uses UART0 on GPIO16/GPIO17 and sends its console output through USB Serial/JTAG. The ESP currently joins the provisioned `beaver` network and keeps `Rabbit-Test-6905` available as a fallback AP. The LCD shows live workout status and the Beaver DHCP address. The checked-in Coach On Deck browser UI is served at `/`, with repeatable pace/sprint set commands and NVS-backed saved sets; `/test` retains the manual diagnostics page. DeckScript plan transfer and surge pacing are flashed and passed paired-device HTTP/UART smoke tests; full browser and pool-side acceptance, external LEDs/audio, protected configuration editing, and hardware qualification remain unfinished. See [firmware/README.md](firmware/README.md) for build and diagnostic commands.

On this development host, the RP2350 appears as `/dev/ttyACM0` and the ESP32-C6 as `/dev/ttyACM1`. With the C++ firmware flashed, their stable serial-device names are:

```text
/dev/serial/by-id/usb-Raspberry_Pi_Pico_509E0DF26DCF56B6-if00
/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_CC:BA:97:F2:69:04-if00
```

The 2026-09-25 paired-device smoke test passed: the ESP32-C6 USB console
reported `Rabbit RP2350 UART link: ready`, and the RP2350 USB `status` command
reported decoded UART requests and responses. The latest station/AP test joined
`beaver` at `192.168.0.178` and returned HTTP 200 for `/`, the browser assets,
pool and saved-set data, and `/api/set-status`. That DHCP address may change;
use the address displayed on the LCD. The fallback AP remains at
`http://192.168.4.1/` after joining `Rabbit-Test-6905`.

## Bluetooth speaker audio

Open **Deck tools → Audio output** in the ESP32 web interface. Audio uses the
same menu-row and Back-button navigation as the other settings.
Choose **Bluetooth speaker** to replace the buzzer or **Buzzer + speaker** for
both outputs. Output selection applies immediately. The volume slider applies
changes automatically as you move it; there is no Save button. Volume is 0–100
and saved volume is preserved. A GPIO active buzzer has a fixed level and is muted
at 0%.

The screen uses a familiar Bluetooth device picker: **My devices** keeps up to eight saved
speakers, including ones that are off or disconnected. Tap a saved device to reconnect
without scanning. Selecting another speaker, closing the page, or restarting either
board keeps all saved devices and their sound confirmations. **Forget** removes only
the device beside that control; forgetting an inactive device keeps the active speaker connected. To add a speaker:

1. Tap **Add speaker** to show **Nearby devices** and pairing instructions.
2. Power on the speaker and hold its Bluetooth button until the pairing indicator
   flashes. Disconnect it from your phone if needed. Tap **Scan** when ready.
   [MEGABOOM 3 pairing instructions](https://support.logi.com/hc/en-gb/articles/18601350410775-Getting-Started-MEGABOOM-3).
3. Tap the desired device in the list to connect directly. Bluetooth addresses
   distinguish devices with identical names; unnamed audio devices can also connect.
4. Once **Connected**, tap **Test 3 beeps**. Adjust volume and test again if needed.
5. Tap **I heard them** to record your sound confirmation. **I didn’t hear them**
   shows guidance to raise volume and repeat the test without confirming.

A scan searches Bluetooth Classic audio devices for about 20 seconds, then resolves
missing names, with a 45-second overall limit. It holds up to 16 devices. **Scan again**
starts fresh; **Stop scan** keeps the current results. Scanning keeps the existing
speaker association and output settings. A rejected Connect request changes neither
output nor volume. An unsuccessful or cancelled new connection retains the previous
saved speaker. **Close** or the screen's Back button stops discovery and cancels an unfinished
connection. Tap the saved speaker to reconnect. An unavailable saved speaker's
automatic reconnect does not block adding another device.
Setup/testing require an idle workout; discovery, connection, and active tests block
workout start. Errors remain visible until the next action.

Bluetooth Classic A2DP/SBC and AVRCP volume control run on the **RP2350B Plus W's CYW43439**.
The ESP32-C6 serves the UI and forwards commands over the existing UART link.
The workout's existing Audio choice gates the same beep signal for both outputs.
Bluetooth speaker buffering adds playback delay; pool-side timing still needs qualification.
While Bluetooth output is connected and unmuted, the RP2350 keeps A2DP streaming and
sends a quiet one-second 80 Hz maintenance pulse every 30 seconds, starting on connection.
The pulse is 1/128 of the workout tone's amplitude and fades in and out to avoid clicks.
It also runs between reps with five- or ten-minute gaps and while idle, to address
speakers that power down despite receiving silent packets. Workout/test beeps take
priority. Volume 0 mutes the pulse; Off, buzzer-only output, or Disconnect stops it.
The exact signal level needed to reset a speaker's inactivity timer is model-dependent;
the MEGABOOM's physical long-gap check remains to be verified. The keep-alive build
is flashed on the RP2350. Three native suites pass, including eleven-minute PCM
checks with five- and ten-minute rep gaps; a round trip through the actual Bluedroid
SBC encoder/decoder preserves the quiet pulse. Live UART/HTTP checks retained both
saved speakers and 49% volume, with the Samsung streaming and no fault recovery reported.
The buzzer remains available if a verified `RABBIT_AUDIO_PIN` is configured.
The development build has that pin disabled.

Output mode, volume, the selected speaker, the saved-device list, each device's sound
confirmation, and pairing keys persist on the RP2350. A full saved-device list does
not evict older speakers: forget one before adding another. Upgrading imports earlier
pairing keys, including devices replaced by the old single-speaker setting. Those keys
contain addresses only; names are recovered when the devices are reachable, and an
old confirmation overwritten by the previous firmware cannot be reconstructed. Settings saves wait until the workout is idle; the API reports pending
saves and the speaker's acknowledged volume. A saved speaker reconnects automatically
while idle when Bluetooth output is enabled. Disconnect suppresses reconnect until
requested again (or the controller restarts); Forget removes the association and its
link key. Confirmation requires a completed test on the currently streaming speaker;
a disconnected or interrupted test cannot be confirmed.

ESP32 HTTP API (commands return current audio status; Devices returns a discovery page):

| Method | Route | Body / query |
| --- | --- | --- |
| GET | `/api/audio/status` | none |
| GET | `/api/audio/saved?offset=0` | saved devices, offset 0–8; up to four results per page |
| POST | `/api/audio/config` | `{"mode":"bluetooth","volume":50}`; modes `off`, `buzzer`, `bluetooth`, `both` |
| POST | `/api/audio/scan` | empty |
| GET | `/api/audio/devices?offset=0` | offset 0–16; up to four results per page |
| POST | `/api/audio/scan-stop` | empty |
| POST | `/api/audio/connect` | `{"address":"C0:28:8D:DC:C9:3D","mode":"bluetooth","volume":49}`; scanned or saved address, mode `bluetooth` or `both` |
| POST | `/api/audio/test` | empty |
| POST | `/api/audio/confirm` | empty; only after a completed speaker test |
| POST | `/api/audio/disconnect` | empty |
| POST | `/api/audio/forget` | `{"address":"C0:28:8D:DC:C9:3D"}` removes that saved speaker; empty body removes the selected speaker |
| POST | `/api/audio/pair` | Legacy exact-name pairing; `{"name":"MEGABOOM 3"}`, empty body uses that name |

Status includes `mode`, `volume`, `connection`, `name`, `address`, `supported`,
`buzzerAvailable`, `paired`, `volumeConfirmed`, `actualVolume`, `testActive`,
`settingsPending`, `soundConfirmed`, `workoutRunning`, and `lastError`.
`actualVolume` is null until acknowledged. Device pages include `scanning`, `full`,
`total`, `devices` (name/address/rssi), and `next` (next offset or null). Saved pages
include `total`, `full`, `devices` (name/address/soundConfirmed), and `next`.
Invalid payloads return 400, unavailable/busy commands 409, unavailable Bluetooth 501,
and a failed UART exchange 503. Scan and Connect are asynchronous; poll Devices and
Status respectively. UI requests have an eight-second response timeout. Slider changes are debounced
200 ms and sent in order; the final slider value is applied even if it changes
while an earlier request is in flight. Output selections apply immediately.

The RP2350 UART API uses the existing version-1 CRC/COBS frames and Hello session.
Message IDs 12–17 remain AudioStatus, AudioConfigure, AudioPair, AudioDisconnect,
AudioTest, and AudioForget. IDs 18–22 add AudioScan, AudioDevices, AudioConnect,
AudioConfirm, and AudioScanStop. IDs 23–24 add AudioSaved and AudioForgetDevice.
Saved takes a one-byte offset; ForgetDevice takes a six-byte address. Configure carries mode/volume bytes; legacy Pair
carries 1–48 name bytes without NUL/control characters. Devices takes a one-byte
offset; Connect takes `[address[6], mode=2|3, volume=0..100]`. Other commands have
empty payloads. Success returns AudioStatus except Devices, which returns AudioDevices.
Request/session IDs are preserved; failures use the existing Reject format.
The status payload remains `[version=1, mode, volume, connection, flags, actualVolume,
lastError, nameLength, address[6], name[nameLength]]`. Connection values 0–6 are
disabled, idle, scanning, connecting, connected, streaming, error. Flag bits 0–7
are supported, buzzer available, paired, volume confirmed, test active, save pending,
sound confirmed, workout running. Unknown actual volume is 255.
Devices payload is `[version=1, scanning, full, total, offset, count, records...]`;
Saved uses the same page header with scanning=0 and replaces RSSI with a 0/1 sound-confirmed byte.
Discovery records are `[nameLength, address[6], signedRssi, name[nameLength]]` (RSSI 0 means
unknown). Four maximum-length records fit in 230 bytes. UART retries reuse IDs and
cannot restart scans, connections, tests, or confirmations. Version-2 audio settings
retain compatibility with previously saved version-1 settings and Bluetooth keys.

Radio GPIOs are GP36 (enable), GP37 (data/host wake), GP38 (CS), and GP39 (clock),
verified against the [Waveshare schematic](https://files.waveshare.com/wiki/RP2350B-Plus-W/RP2350B-Plus-W.pdf).
The LED output uses PIO1 to keep its low GPIO range separate from the radio's high GPIOs.
Large LED/UART buffers reside in static RAM; the foreground stack reserves 4 KiB.
The radio uses foreground CYW43 polling so Bluetooth callbacks and UART commands share
one execution context. Both AVRCP roles are initialized to handle the speaker's control
messages. A four-second watchdog recovers a stalled controller; USB `status` reports
the last recovery stage and captured fault location.
Pairing/settings use the SDK's two flash TLV banks near the end of the 16 MiB flash;
avoid erasing that storage when preserving saved audio settings.

The 2026-10-04 saved-device build is installed on both boards. Native protocol/workout
checks pass (2/2), including saved-list storage validation, pagination, retaining
multiple speakers, and forgetting one device without removing another. Mobile browser
checks cover retained device lists, reconnecting without a scan, per-device Forget,
automatic volume/output changes, connection errors, sound confirmation, workout gating,
scan cancellation, and Back navigation. The installed ESP32 UI displays both the
Samsung Soundbar MS650 and the recovered MEGABOOM pairing, with separate Connect/Forget
controls; the disconnected MEGABOOM remains listed after a page reload. A further
RP2350 reboot retained both records, the Samsung sound confirmation, selected speaker,
and 49% volume; the soundbar resumed streaming. The MEGABOOM's old link key contains
its address but not the name or confirmation overwritten by the old single-device
setting. Earlier live checks verified automatic MEGABOOM volume changes and real
discovery/cancellation. Pool-side latency remains to be qualified. The ESP32 address
for these checks was `http://192.168.0.166/`; use the current LCD address if DHCP changes it.
