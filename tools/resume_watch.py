"""Resume playback and watch the console for the recovery story."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 90.0

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()

buf = bytearray()
start = time.time()
sent_resume = False
next_poll = start + 20.0
while time.time() - start < seconds:
    if not sent_resume and time.time() - start > 2.0:
        ser.write(b"CMD:UI:PLAYPAUSE\n")  # resume (state was PAUSED)
        ser.flush()
        sent_resume = True
    if time.time() >= next_poll:
        ser.write(b"CMD:METRICS\n")
        ser.flush()
        next_poll += 20.0
    data = ser.read(4096)
    if data:
        buf.extend(data)

ser.close()
sys.stdout.buffer.write(bytes(buf))
