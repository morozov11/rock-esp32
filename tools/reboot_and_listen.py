import subprocess
import time
import serial
import sys

port = sys.argv[1] if len(sys.argv) > 1 else 'COM6'

print(f"Triggering watchdog reset on {port}...")
subprocess.run([
    r'C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe',
    '-m', 'esptool', '-p', port, '--chip', 'esp32p4', '--after', 'watchdog-reset', 'run'
], check=True)

print(f"Opening {port} to read logs...")
ser = serial.Serial(port, 115200, timeout=0.5)
t0 = time.time()
while time.time() - t0 < 15:
    line = ser.readline()
    if line:
        try:
            print(line.decode('utf-8', errors='replace').rstrip())
        except Exception:
            pass
ser.close()
