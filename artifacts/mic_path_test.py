#!/usr/bin/env python3
"""Ground-truth acoustic check: capture the board mic with RE8:BULK while the
WAV plays on the PC, then report per-second RMS to see whether the mic hears
the playback at all."""

import base64
import re
import subprocess
import struct
import time

import serial

PORT = "COM6"
WAV = r"artifacts\live_probe\segments\20261025T110305Z_re8-live-commands-near_0005_cue02.wav"

DATA = re.compile(r"RE8CAP data id=\d+ seq=\d+ ([A-Za-z0-9+/=]+)")
END = re.compile(r"RE8CAP end id=\d+")

dev = serial.Serial()
dev.port = PORT
dev.baudrate = 115200
dev.timeout = 0.2
dev.dtr = False
dev.rts = False
dev.open()
time.sleep(0.3)
dev.reset_input_buffer()

dev.write(b"CMD:RE8:BULK:probewav:6\n")
dev.flush()
time.sleep(1.5)
subprocess.run(
    ["powershell", "-NoProfile", "-Command",
     f"(New-Object Media.SoundPlayer '{WAV}').PlaySync()"],
    check=False,
)

chunks = []
deadline = time.monotonic() + 30
while time.monotonic() < deadline:
    raw = dev.readline().decode("utf-8", errors="replace").strip()
    if not raw:
        continue
    m = DATA.search(raw)
    if m:
        chunks.append(base64.b64decode(m.group(1)))
    if END.search(raw):
        break
dev.close()

pcm = b"".join(chunks)
print(f"captured {len(pcm)} bytes = {len(pcm) / 2 / 16000:.2f}s")
samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
per_second = 16000
for sec in range(len(samples) // per_second):
    window = samples[sec * per_second:(sec + 1) * per_second]
    rms = (sum(s * s for s in window) // len(window)) ** 0.5
    peak = max(abs(s) for s in window)
    print(f"sec {sec}: rms={rms:.0f} peak={peak}")
