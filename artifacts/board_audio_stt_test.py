#!/usr/bin/env python3
"""Capture what the board microphone actually records while a command plays,
save it as a WAV, and report sample statistics (dropouts, clipping, DC)."""

import base64
import re
import struct
import subprocess
import sys
import time
import wave

import serial

PORT = "COM6"
WAV = r"artifacts\tts\stop.wav"
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

dev.write(b"CMD:RE8:BULK:stttest:7\n")
dev.flush()
time.sleep(1.5)
subprocess.run(
    ["powershell", "-NoProfile", "-Command",
     f"(New-Object Media.SoundPlayer '{WAV}').PlaySync()"],
    check=False,
)

chunks = []
deadline = time.monotonic() + 40
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
samples = struct.unpack(f"<{len(pcm) // 2}h", pcm)
out = sys.argv[1] if len(sys.argv) > 1 else "artifacts/board_mic_capture.wav"
with wave.open(out, "wb") as w:
    w.setnchannels(1)
    w.setsampwidth(2)
    w.setframerate(16000)
    w.writeframes(pcm)

zero_runs = 0
current_run = 0
clipped = sum(1 for s in samples if abs(s) >= 32000)
for s in samples:
    if s == 0:
        current_run += 1
    else:
        if current_run >= 160:
            zero_runs += 1
        current_run = 0
mean = sum(samples) / max(1, len(samples))
rms = (sum(s * s for s in samples) / max(1, len(samples))) ** 0.5
print(f"saved {out}: {len(samples)} samples ({len(samples)/16000:.2f}s), "
      f"rms={rms:.0f} mean={mean:.0f} peak={max(map(abs, samples))} "
      f"clipped={clipped} zero_runs>=10ms={zero_runs}")
