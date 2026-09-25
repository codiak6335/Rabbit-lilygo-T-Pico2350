# Rabbit Desktop Emulators

This adds a local emulator for the LED strand, pool-bottom tracing, OLED/TFT
display text, and the existing Rabbit web interface.

## Run

From repo root:

```bash
python3 emulator/run_desktop_emulator.py
```

Then open:
- Emulator view: `http://127.0.0.1:8080/emulator`
- Rabbit web UI: `http://127.0.0.1:8080`

## Notes

- Hardware-specific MicroPython modules are stubbed from `emulator/stubs/`.
- Absolute runtime paths like `/db/pools.json` are redirected to `./db/pools.json`.
- `main.py` still runs as-is; the launcher only patches runtime behavior for desktop emulation.
