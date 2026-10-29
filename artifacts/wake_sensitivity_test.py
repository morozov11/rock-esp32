#!/usr/bin/env python3
"""Controlled wake sensitivity test: replays the recorded wake phrase N times
over the PC speakers and counts crossings, gated events and voice handoffs."""

import subprocess
import sys
import time

import serial

PORT = "COM6"
WAV = sys.argv[1] if len(sys.argv) > 1 else r"artifacts\live_probe\segments\20261025T110305Z_re8-live-commands-near_0005_cue02.wav"
REPEATS = int(sys.argv[2]) if len(sys.argv) > 2 else 3

lines = []
dev = serial.Serial()
dev.port = PORT
dev.baudrate = 115200
dev.timeout = 0.2
dev.dtr = False
dev.rts = False
dev.open()
time.sleep(0.3)
dev.reset_input_buffer()

for i in range(REPEATS):
    print(f">>> replay {i + 1}/{REPEATS}", flush=True)
    lines.append(f">>> replay {i + 1}")
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"(New-Object Media.SoundPlayer '{WAV}').PlaySync()"],
        check=False,
    )
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if not raw:
            continue
        if any(m in raw for m in ("RE8WAKE", "rock-voice", "rock-gui")):
            lines.append(raw)
            print(raw[:220], flush=True)
        if "session finished" in raw:
            break

dev.write(b"CMD:METRICS\n")
dev.flush()
deadline = time.monotonic() + 6
while time.monotonic() < deadline:
    raw = dev.readline().decode("utf-8", errors="replace").strip()
    if "metrics_wake" in raw:
        lines.append(raw)
        print(raw, flush=True)
        break
dev.close()
with open("artifacts/wake_sensitivity_log.txt", "w", encoding="utf-8") as f:
    f.write("\n".join(lines) + "\n")
