import serial
import time
import sys

ser = serial.Serial()
ser.port = 'COM6'
ser.baudrate = 115200
ser.timeout = 0.1
ser.dtr = False
ser.rts = False
ser.open()

def send_and_wait_ack(cmd, timeout=3.0):
    ser.reset_input_buffer()
    t0 = time.time()
    ser.write((cmd + '\n').encode('utf-8'))
    ser.flush()
    while time.time() - t0 < timeout:
        line = ser.readline().decode('utf-8', errors='ignore').strip()
        if f"Serial command received: '{cmd}'" in line:
            elapsed = (time.time() - t0) * 1000
            print(f"ACK for {cmd} in {elapsed:.1f} ms: {line}")
            return elapsed
    print(f"TIMEOUT waiting for {cmd}")
    return None

print("Checking button response latency in STOPPED state with Chromecast active...")
latencies = []
for i in range(5):
    time.sleep(1)
    ms = send_and_wait_ack("CMD:UI:VOL+")
    if ms is not None:
        latencies.append(ms)

print(f"Latencies: {latencies}")
if latencies:
    print(f"Average latency: {sum(latencies)/len(latencies):.1f} ms (max: {max(latencies):.1f} ms)")

ser.close()
