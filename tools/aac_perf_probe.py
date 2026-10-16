"""Measure the AAC transcode feeder: start a station, poll CMD:METRICS, capture logs.

Usage: aac_perf_probe.py [port] [station_id] [seconds] [out_path]
Defaults: COM6 rb-71e6e2bc-7143-4e06-9e6c-61aebe046966 85 telemetry/aac_perf_window.log
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
station = sys.argv[2] if len(sys.argv) > 2 else "rb-71e6e2bc-7143-4e06-9e6c-61aebe046966"
seconds = int(sys.argv[3]) if len(sys.argv) > 3 else 85
out_path = sys.argv[4] if len(sys.argv) > 4 else "telemetry/aac_perf_window.log"

ser = serial.Serial()
ser.port = port
ser.baudrate = 115200
ser.timeout = 1
# USB-Serial/JTAG: keep control lines deasserted so opening the port
# never resets or halts the running firmware.
ser.dtr = False
ser.rts = False
ser.open()

lines = []


def pump(duration: float):
    end = time.time() + duration
    buf = bytearray()
    while time.time() < end:
        data = ser.read(4096)
        if data:
            buf.extend(data)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                if text:
                    lines.append(text)
                    print(text)


# Quiet window: skip boot leftovers, then start the station.
pump(3)
print(f"=== starting station {station} ===")
ser.write(f"CMD:UI:STATIONID:{station}\n".encode())

start = time.time()
next_metrics = start + 10
while time.time() - start < seconds:
    pump(1)
    if time.time() >= next_metrics:
        ser.write(b"CMD:METRICS\n")
        next_metrics += 10

ser.close()
with open(out_path, "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
print(f"saved {len(lines)} lines -> {out_path}", file=sys.stderr)
