#!/usr/bin/env python3
"""Reconnecting serial capture for the Cardputer ADV.

The board re-enumerates its USB-CDC port whenever it resets, so a plain
`pio device monitor` session dies exactly when the interesting part starts.
This script reopens the port automatically, drops the RMT noise spam, and
writes everything else to a timestamped log.

    python tools/cap-serial.py 600                 # 10 min, auto port/log
    python tools/cap-serial.py 600 /tmp/cap.log    # explicit log path
    python tools/cap-serial.py 600 --port COM7     # explicit port

Needs pyserial.  If the system python lacks it, use PlatformIO's interpreter:

    macOS   /opt/homebrew/Cellar/platformio/*/libexec/bin/python tools/cap-serial.py 600
    Windows %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe tools/cap-serial.py 600
"""

import argparse
import glob
import sys
import time

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    sys.exit("pyserial missing - see the docstring for the PlatformIO interpreter")

IGNORE = ("RX buffer too small",)


def find_port():
    ports = sorted(p.device for p in list_ports.comports())
    for candidate in ports:
        if any(key in candidate for key in ("usbmodem", "usbserial", "ACM", "CH34", "CP210", "SLAB")):
            return candidate
    return ports[0] if ports else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("seconds", type=float, nargs="?", default=300)
    parser.add_argument("log", nargs="?", default="/tmp/cap.log")
    parser.add_argument("--port", default=None)
    args = parser.parse_args()

    end = time.time() + args.seconds
    port = args.port
    ser = None
    pending = b""
    last_line = time.time()
    last_note = 0.0
    log = open(args.log, "w", buffering=1)

    def emit(text):
        log.write("%s %s\n" % (time.strftime("%H:%M:%S"), text))

    emit("capture start, %ds, port=%s" % (int(args.seconds), port or "auto"))

    while time.time() < end:
        if ser is None:
            port = port or find_port()
            if not port:
                time.sleep(0.3)
                continue
            try:
                ser = serial.Serial(port, 115200, timeout=0.2)
                emit("--- port opened %s ---" % port)
                pending = b""
                last_line = time.time()
            except Exception as exc:
                emit("open failed: %s" % exc)
                port = None
                time.sleep(0.5)
                continue
        try:
            chunk = ser.read(512)
        except Exception as exc:
            emit("--- read failed, reconnecting: %s ---" % exc)
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            continue

        if not chunk:
            quiet = time.time() - last_line
            if quiet > 3 and time.time() - last_note > 10:
                last_note = time.time()
                emit("--- quiet for %ds ---" % int(quiet))
            continue

        last_line = time.time()
        pending += chunk
        while b"\n" in pending:
            line, pending = pending.split(b"\n", 1)
            text = line.decode("utf-8", "replace").rstrip()
            if not text or any(key in text for key in IGNORE):
                continue
            emit(text)

    emit("capture end")
    if ser is not None:
        ser.close()
    log.close()


if __name__ == "__main__":
    main()
