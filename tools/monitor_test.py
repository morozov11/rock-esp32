import sys
import time
import serial

port = 'COM6'
baud = 115200

print(f'Connecting to {port} @ {baud}...')
try:
    ser = serial.Serial(port, baud, timeout=1)
except Exception as e:
    print(f'Error opening port: {e}')
    sys.exit(1)

# Reset ESP32-P4
ser.setDTR(False)
ser.setRTS(True)
time.sleep(0.1)
ser.setRTS(False)
time.sleep(0.1)

print('Monitoring output for up to 45 seconds...')
start = time.time()
passed = False

while time.time() - start < 45:
    line = ser.readline()
    if line:
        text = line.decode('utf-8', errors='replace').rstrip()
        print(text)
        if 'RE-5 PLAYER HARDWARE VERIFICATION: ALL PASSED!' in text:
            passed = True
            # Read a few more lines to catch finish
            for _ in range(5):
                extra = ser.readline()
                if extra:
                    print(extra.decode('utf-8', errors='replace').rstrip())
            break

ser.close()
if passed:
    print("\nSUCCESS: RE-5 Hardware Verification PASSED!")
    sys.exit(0)
else:
    print("\nTIMEOUT or test incomplete.")
    sys.exit(1)
