"""Provision ESP32-C6 Wi-Fi from the existing Rabbit Wi-Fi settings file.

The password is sent over the attached USB Serial/JTAG port. It is never
printed, passed as a command-line argument, or compiled into the firmware.
"""

import argparse
import json
import time
from pathlib import Path

import serial


def wait_for(port: serial.Serial, expected: bytes, timeout: float) -> bool:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(port.read(port.in_waiting or 1))
        if expected in received:
            return True
        if len(received) > 4096:
            del received[:-1024]
    return False


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="ESP32-C6 USB Serial/JTAG device")
    parser.add_argument("--ssid", default="beaver", help="SSID in db/wifi.json (default: beaver)")
    parser.add_argument(
        "--settings",
        type=Path,
        default=Path(__file__).resolve().parents[1] / "db" / "wifi.json",
        help="Rabbit Wi-Fi settings JSON",
    )
    args = parser.parse_args()

    with args.settings.open(encoding="utf-8") as settings_file:
        entries = json.load(settings_file)["wifis"]
    entry = next((item for item in entries if item.get("ssid") == args.ssid), None)
    if entry is None:
        parser.error(f"SSID {args.ssid!r} not found in settings")

    ssid = entry["ssid"]
    password = entry["password"]
    if not 1 <= len(ssid.encode()) < 32 or not 8 <= len(password.encode()) < 64:
        parser.error("SSID or password length is not supported by the ESP32-C6")
    if any(character in ssid or character in password for character in "\t\r\n"):
        parser.error("SSID and password cannot contain tab or newline characters")

    port = serial.Serial()
    port.port = args.port
    port.baudrate = 115200
    port.timeout = 0.2
    port.dtr = False
    port.rts = False
    try:
        port.open()
        time.sleep(3)  # Opening USB Serial/JTAG can restart the target.
        port.reset_input_buffer()
        port.write(b"PING\n")
        if not wait_for(port, b"Rabbit Wi-Fi provisioning PONG", 3):
            raise SystemExit("ESP provisioning interface did not respond")
        port.write(f"WIFI\t{ssid}\t{password}\n".encode())
        if not wait_for(port, b"Rabbit Wi-Fi credentials saved; restarting", 5):
            raise SystemExit("ESP did not acknowledge Wi-Fi provisioning")
    finally:
        port.close()
    print(f"Saved credentials for {ssid}; ESP32-C6 is restarting.")


if __name__ == "__main__":
    main()
