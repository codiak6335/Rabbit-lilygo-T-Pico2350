# Support Request: T-Pico2350 ESP32-C6 / RP2350 UART Failure

## Summary

We are requesting assistance with the communication link between the ESP32-C6 and RP2350 on a LilyGO T-Pico2350 / T-PicoPro. Both processors boot and can be flashed independently through the reversible USB-C connector, but they do not communicate over their inter-processor UART connection.

This report includes results from our own test firmware and, independently, from the factory firmware supplied in the LilyGO T-Pico2 repository. No knowledge of our application is required to investigate this issue.

## Board and Symptoms

- Board: LilyGO T-Pico2350 / T-PicoPro with RP2350A and ESP32-C6.
- The reversible USB-C connector selects either the RP2350 or ESP32-C6 USB interface as expected.
- The ESP32-C6 is detected as `ESP32-C6FH4` and accepts firmware flashes successfully.
- The RP2350 enters BOOTSEL mode and accepts UF2 firmware successfully.
- Our ESP32-C6 test firmware can run a SoftAP and HTTP server, but requests requiring an RP2350 response return `503 Service Unavailable`.
- The charging-status LED blinks rapidly with no battery connected. Please confirm whether this is expected on this board revision.

## Independent UART Test Firmware

To verify the link directly, we configured our test firmware using the following inter-chip pin mapping:

| RP2350A | ESP32-C6 |
| --- | --- |
| GPIO28 TX | GPIO6 RX |
| GPIO29 RX | GPIO7 TX |
| GPIO26 CTS | GPIO5 CTS |
| GPIO27 RTS | GPIO4 RTS |

The RP2350 was configured for UART0 on GPIO28/GPIO29. The ESP32-C6 was configured for UART1 at 115200 baud on GPIO7/GPIO6. The ESP32-C6 reports successful writes of complete 19-byte UART protocol frames, but RP2350 diagnostics see no complete frames and no replies are returned.

## Factory Firmware Reproduction

To determine whether the issue was specific to our software, we flashed the official LilyGO factory firmware supplied in the T-Pico2 repository to both processors:

1. Flashed `firmware/factory_ESP32C6-4MB.bin` to the ESP32-C6 at address `0x0`. The write and verification completed successfully.
2. Flashed `firmware/T-Lilygo-rp2350_ESP32C6_V1.2.uf2` to the RP2350 BOOTSEL drive. The copy completed successfully.
3. Opened the RP2350 serial output at 115200 baud.

The factory RP2350 firmware reported:

```
WiFi Module is not online !
```

This occurred while both LilyGO factory images were installed. We then restored our own test firmware.

## Requested Support

Please advise whether this result indicates a hardware fault, an affected board revision, or an additional required hardware or firmware initialization step. In particular, please confirm:

1. The correct ESP32-C6 ↔ RP2350 UART pin mapping and UART peripheral settings for this board revision.
2. Whether the factory RP2350 firmware should detect the ESP32-C6 after the supplied factory ESP32-C6 image is flashed, and whether `WiFi Module is not online !` is an expected result.
3. The meaning of the rapid green charging-status LED blink when no battery is attached.
4. Whether the board should be replaced under warranty based on the failed factory inter-processor test.
