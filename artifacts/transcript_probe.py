#!/usr/bin/env python3
"""Probe voice commands with TTS WAVs and capture STT transcripts."""

import subprocess
import sys
import time

import serial

PORT = "COM6"
NAMES = sys.argv[1:] or ["rock", "stop"]

KEEP = ("rock-voice", "rock-control", "rock-gui", "rock-serial: metrics_cast")


def main() -> None:
    results = []
    dev = serial.Serial()
    dev.port = PORT
    dev.baudrate = 115200
    dev.timeout = 0.2
    dev.dtr = False
    dev.rts = False
    dev.open()
    time.sleep(0.3)
    dev.reset_input_buffer()
    for name in NAMES:
        wav = rf"artifacts\tts\{name}.wav"
        print(f"--- probe {name} ---", flush=True)
        dev.write(b"CMD:UI:VOICE\n")
        dev.flush()
        time.sleep(1.0)
        subprocess.run(
            ["powershell", "-NoProfile", "-Command",
             f"(New-Object Media.SoundPlayer '{wav}').PlaySync()"],
            check=False,
        )
        deadline = time.monotonic() + 55
        while time.monotonic() < deadline:
            raw = dev.readline().decode("utf-8", errors="replace").strip()
            if not raw:
                continue
            if any(m in raw for m in KEEP):
                print(raw, flush=True)
                results.append(raw)
            if "session finished" in raw:
                break
        time.sleep(2)
    dev.write(b"CMD:METRICS\n")
    dev.flush()
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if "rock-gui: ui " in raw:
            print(raw, flush=True)
            results.append(raw)
            break
    dev.close()
    with open("artifacts/transcript_probe_log.txt", "w", encoding="utf-8") as f:
        f.write("\n".join(results) + "\n")


if __name__ == "__main__":
    main()
