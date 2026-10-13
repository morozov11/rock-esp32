"""Play search rows until a stream dies, then verify the catalog stays clean."""
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
send("CMD:UI:SEARCH:black metal")
drain(6.0)

died = False
for row in range(10):
    send(f"CMD:UI:STATION:{row}")
    out = drain(18.0)
    if b"Stream source ended" in out or b"ROCK_PLAYER_UI_ERROR" in out or b"error" in out.lower():
        died = True
        break

print(f"DIED={died}")
send("CMD:UI:CATALOG")
drain(6.0)
send("CMD:METRICS")
drain(3.0)
ser.close()
sys.stdout.buffer.write(bytes(buf))
