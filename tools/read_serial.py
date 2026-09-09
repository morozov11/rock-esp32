import serial
import time
import sys

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 5.0

try:
    s = serial.Serial(port, 115200, timeout=0.2)
except Exception as e:
    print(f"Error opening {port}: {e}")
    sys.exit(1)

if "--reset" in sys.argv:
    s.dtr = False
    s.rts = True
    time.sleep(0.1)
    s.rts = False
    time.sleep(0.1)

t0 = time.time()

while time.time() - t0 < duration:
    line = s.readline()
    if line:
        try:
            print(line.decode("utf-8", errors="replace").rstrip())
        except Exception:
            pass
