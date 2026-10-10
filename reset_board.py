import serial
import time
import sys

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

s = serial.Serial()
s.port = 'COM6'
s.baudrate = 115200
s.timeout = 0.5
s.dtr = False
s.rts = False
s.open()

print("Resetting board...")
s.dtr = True
s.rts = True
time.sleep(0.1)
s.dtr = False
s.rts = False
time.sleep(0.2)

t0 = time.time()
while time.time() - t0 < 15:
    line = s.readline()
    if line:
        text = line.decode('utf-8', errors='replace').rstrip('\r\n')
        print(text)
s.close()
