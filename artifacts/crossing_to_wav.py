#!/usr/bin/env python3
"""Dump the last wake crossing's pre-roll audio to a WAV for analysis."""

import base64
import re
import struct
import sys
import time
import wave

import serial

DATA = re.compile(r"RE8WAKE dump data id=\d+ seq=(\d+) ([A-Za-z0-9+/=]+)")


def main() -> None:
    out = sys.argv[1] if len(sys.argv) > 1 else "artifacts/last_crossing.wav"
    dev = serial.Serial()
    dev.port = "COM6"
    dev.baudrate = 115200
    dev.timeout = 0.3
    dev.dtr = False
    dev.rts = False
    dev.open()
    time.sleep(0.3)
    dev.write(b"CMD:RE8:WAKE:LAST\n")
    dev.flush()
    chunks = {}
    deadline = time.time() + 20
    while time.time() < deadline:
        line = dev.readline().decode("utf-8", errors="replace").strip()
        m = DATA.search(line)
        if m:
            chunks[int(m.group(1))] = base64.b64decode(m.group(2))
    dev.close()
    pcm = b"".join(chunks[k] for k in sorted(chunks))
    if not pcm:
        print("no dump captured")
        return
    with wave.open(out, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm)
    s = struct.unpack(f"<{len(pcm) // 2}h", pcm)
    rms = (sum(x * x for x in s) / len(s)) ** 0.5
    print(f"saved {out}: {len(s)} samples, rms={rms:.0f} peak={max(map(abs, s))}")


if __name__ == "__main__":
    main()
