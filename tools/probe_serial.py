"""Send CMD: lines to the rock console and capture the responses."""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
commands = [a for a in sys.argv[2:] if a]
listen_s = float(6)

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
ser.dtr = False
ser.rts = False
ser.open()
# Give the port a moment to settle; USB-JTAG needs no handshake.
time.sleep(0.2)

buf = bytearray()


def drain(seconds: float) -> None:
    end = time.time() + seconds
    while time.time() < end:
        data = ser.read(4096)
        if data:
            buf.extend(data)


drain(0.5)  # clear anything pending
for cmd in commands:
    ser.write((cmd + "\n").encode())
    ser.flush()
    drain(listen_s)

ser.close()
sys.stdout.buffer.write(bytes(buf))
