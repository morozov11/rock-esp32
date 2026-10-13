"""Scan all catalog rows for AAC; stress pause/resume on AAC (or current)."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()

buf = bytearray()


def drain(seconds: float) -> bytes:
    end = time.time() + seconds
    chunk = bytearray()
    while time.time() < end:
        data = ser.read(4096)
        if data:
            chunk.extend(data)
            buf.extend(data)
    return bytes(chunk)


def send(cmd: str) -> None:
    ser.write((cmd + "\n").encode())
    ser.flush()


drain(0.5)
send("CMD:UI:CATALOG")
drain(4.0)

aac_row = None
for row in range(20):
    send(f"CMD:UI:STATION:{row}")
    out = drain(6.0)
    if b"audio/aac" in out or b"audio/aacp" in out or b"audio/x-aac" in out:
        aac_row = row
        break

if aac_row is None:
    print("NO_AAC_ROW_FOUND; stressing current station")
else:
    print(f"AAC_ROW={aac_row}")
drain(8.0)  # reach PLAYING

for i in range(6):
    send("CMD:UI:PLAYPAUSE")
    drain(5.0)
    send("CMD:UI:PLAYPAUSE")
    drain(7.0)

send("CMD:METRICS")
drain(3.0)
ser.close()
sys.stdout.buffer.write(bytes(buf))
