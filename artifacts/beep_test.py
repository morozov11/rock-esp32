#!/usr/bin/env python3
"""Beep from the PC sound card and capture the board mic to verify the
acoustic stimulus path end to end."""

import base64
import re
import struct
import subprocess
import sys
import time

import serial

DATA = re.compile(r"RE8CAP data id=\d+ seq=\d+ ([A-Za-z0-9+/=]+)")
END = re.compile(r"RE8CAP end id=\d+")


def main() -> None:
    dev = serial.Serial()
    dev.port = "COM6"
    dev.baudrate = 115200
    dev.timeout = 0.2
    dev.dtr = False
    dev.rts = False
    dev.open()
    time.sleep(0.3)
    dev.reset_input_buffer()
    dev.write(b"CMD:RE8:BULK:beeptest:5\n")
    dev.flush()
    time.sleep(1.0)
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         "[console]::beep(880,400); [console]::beep(880,400)"],
        check=False,
    )
    chunks = []
    events = []
    deadline = time.time() + 30
    while time.time() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if not raw:
            continue
        if "RE8CAP" in raw and not DATA.search(raw):
            events.append(raw)
        m = DATA.search(raw)
        if m:
            chunks.append(base64.b64decode(m.group(1)))
        if END.search(raw):
            break
    dev.close()
    for e in events[:6]:
        print("event:", e)
    pcm = b"".join(chunks)
    print(f"captured {len(pcm)} bytes")
    s = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    for i in range(len(s) // 8000):
        w = s[i * 8000:(i + 1) * 8000]
        rms = (sum(x * x for x in w) / len(w)) ** 0.5
        print(f"0.5s {i}: rms={rms:.0f} peak={max(map(abs, w))}")


if __name__ == "__main__":
    main()
