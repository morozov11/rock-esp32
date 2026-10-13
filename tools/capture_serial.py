"""Capture raw ESP32 console output without toggling DTR/RTS (no reset)."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 60
out_path = sys.argv[3] if len(sys.argv) > 3 else "-"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
# USB-Serial/JTAG: keep control lines deasserted so opening the port
# never resets or halts the running firmware.
ser.dtr = False
ser.rts = False
ser.open()
deadline = time.time() + seconds
chunks = []
while time.time() < deadline:
    data = ser.read(4096)
    if data:
        chunks.append(data)

ser.close()
blob = b"".join(chunks)
if out_path == "-":
    sys.stdout.buffer.write(blob)
else:
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"captured {len(blob)} bytes -> {out_path}", file=sys.stderr)
