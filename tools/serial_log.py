"""Capture ESP32 serial output for N seconds after a reset.

Usage: python tools/serial_log.py COM14 25 [out.log]
Requires pyserial (bundled with the ESP-IDF python env).
"""
import sys
import time

import serial


def main() -> int:
    port = sys.argv[1] if len(sys.argv) > 1 else "COM14"
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    out_path = sys.argv[3] if len(sys.argv) > 3 else ""

    ser = serial.Serial(port, 115200, timeout=0.2)
    # CH340 DTR/RTS are wired to IO0/EN: pulse EN to reboot into the app.
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.15)
    ser.setRTS(False)
    time.sleep(0.1)
    ser.reset_input_buffer()

    chunks = []
    fout = open(out_path, "w", encoding="utf-8") if out_path else None
    deadline = time.time() + seconds
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            text = data.decode("utf-8", "replace")
            chunks.append(text)
            if fout is not None:
                fout.write(text)
                fout.flush()
    ser.close()
    if fout is not None:
        fout.close()

    blob = "".join(chunks)
    print(blob)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
