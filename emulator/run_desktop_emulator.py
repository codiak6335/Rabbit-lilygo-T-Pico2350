import builtins
import importlib
import os
import pathlib
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def _patch_time():
    if not hasattr(time, "ticks_ms"):
        time.ticks_ms = lambda: int(time.monotonic() * 1000)
    if not hasattr(time, "ticks_diff"):
        time.ticks_diff = lambda a, b: int(a - b)
    if not hasattr(time, "sleep_ms"):
        time.sleep_ms = lambda ms: time.sleep(max(0, ms) / 1000.0)


def _redirect_abs_path(path):
    mapping = {
        "/db/": ROOT / "db",
        "/css/": ROOT / "css",
        "/js/": ROOT / "js",
        "/static/": ROOT / "static",
    }
    for prefix, target in mapping.items():
        if isinstance(path, str) and path.startswith(prefix):
            return str(target / path[len(prefix):])

    if path == "/db":
        return str(ROOT / "db")
    if path == "/css":
        return str(ROOT / "css")
    if path == "/js":
        return str(ROOT / "js")
    if path == "/static":
        return str(ROOT / "static")

    # In emulator mode, app code often opens relative paths
    # (e.g. "index.html", "favicon.ico"). Resolve them from project root
    # so behavior does not depend on the process working directory.
    if isinstance(path, str) and not os.path.isabs(path):
        candidate = ROOT / path
        if candidate.exists():
            return str(candidate)

    return path


def _patch_paths():
    original_open = builtins.open

    def redirected_open(file, *args, **kwargs):
        return original_open(_redirect_abs_path(file), *args, **kwargs)

    builtins.open = redirected_open


def _patch_microdot_run():
    microdot = importlib.import_module("microdot")
    original_run = microdot.Microdot.run

    def patched_run(self, host="127.0.0.1", port=80, debug=False, ssl=None):
        emulator_port = int(os.getenv("RABBIT_EMULATOR_APP_PORT", "8080"))
        print(f"[emulator] overriding app.run(port={port}) -> port={emulator_port}")
        return original_run(self, host=host, port=emulator_port, debug=debug, ssl=ssl)

    microdot.Microdot.run = patched_run


def main():
    sys.path.insert(0, str(ROOT))
    sys.path.insert(0, str(ROOT / "emulator" / "stubs"))

    _patch_time()
    _patch_paths()
    _patch_microdot_run()

    print("[emulator] view: http://127.0.0.1:8080/emulator")
    print("[emulator] app:  http://127.0.0.1:8080")

    import main  # noqa: F401


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n[emulator] stopped by user (SIGINT)")
