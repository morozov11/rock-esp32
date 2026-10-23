import serial
import time
import sys
import re

ser = serial.Serial()
ser.port = 'COM6'
ser.baudrate = 115200
ser.timeout = 0.2
ser.dtr = False
ser.rts = False
ser.open()

def send(cmd):
    print(f"\n---> SENDING: {cmd} at {time.strftime('%H:%M:%S')}")
    ser.write((cmd + '\n').encode('utf-8'))
    ser.flush()

def wait_for(pattern, timeout_s):
    rx = re.compile(pattern)
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if line:
            line_clean = re.sub(r"https?://[^\s'\"]+", "<redacted-uri>", line)
            if 'WSS tx: ' not in line_clean and 'WSS rx: ' not in line_clean:
                print(line_clean)
            m = rx.search(line_clean)
            if m:
                return m
    return None

print("Testing Station Switching while Cast is active...")

# 1. Start SomaFM Metal Detector
print("\n[STEP 1] Starting somafm-metal-detector...")
send("CMD:UI:STATIONID:somafm-metal-detector")
m = wait_for(r"Relay streamed \d+ bytes to Chromecast", 35)
if not m:
    print("Failed to start somafm-metal-detector on Chromecast")
else:
    print("SomaFM streaming on Chromecast successfully!")

time.sleep(3)

# 2. Switch directly to another station via catalog row or STATIONID
# Let's switch to 'somafm-groovesalad' or another station
print("\n[STEP 2] Switching directly to 'somafm-groovesalad'...")
send("CMD:UI:STATIONID:somafm-groovesalad")
m = wait_for(r"Relay streamed \d+ bytes to Chromecast", 35)
if not m:
    print("FAILED: Switching to somafm-groovesalad did not stream to Chromecast!")
else:
    print("SUCCESS: Station switched and streaming on Chromecast without stop/start!")

ser.close()
