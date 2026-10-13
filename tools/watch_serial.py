"""Watch the console for a window while polling CMD:METRICS."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 120.0
poll_every = float(sys.argv[3]) if len(sys.argv) > 3 else 10.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()

buf = bytearray()
start = time.time()
next_poll = start + 2.0
while time.time() - start < seconds:
    if time.time() >= next_poll:
        ser.write(b"CMD:METRICS\n")
        ser.flush()
        next_poll += poll_every
    data = ser.read(4096)
    if data:
        buf.extend(data)

ser.close()
sys.stdout.buffer.write(bytes(buf))
