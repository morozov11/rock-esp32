#!/usr/bin/env python3
"""Command matrix over CMD:UI:VOICE with TTS WAVs played through the PC
speakers; captures the exact server error code for every session."""

import subprocess
import sys
import time

import serial

PORT = "COM6"
LOG = sys.argv[1] if len(sys.argv) > 1 else r"artifacts\matrix_probe_log.txt"
PHRASES = [
    "rock",     # search: expect success + Cast autoplay (15%)
    "quieter",  # 15% -> 10%
    "louder",   # 10% -> 15%
    "like",     # favourite while playing: expect success
    "next",     # navigation: capability_not_supported (manifest-gated, expected)
    "stop",     # expect success
    "start",    # resume from stopped: expect success + Cast playback
    "stop",     # final stop
]

KEEP = ("rock-voice", "rock-control", "rock-gui", "Guru Meditation", "abort()",
        "rock-serial: metrics", "rock-player: stream metrics", "rock_relay: ICY")


def run_phrase(dev: serial.Serial, name: str, lines: list[str]) -> None:
    wav = rf"artifacts\tts\{name}.wav"
    lines.append(f">>> probe {name}")
    print(f"--- probe {name} ---", flush=True)
    dev.write(b"CMD:UI:VOICE\n")
    dev.flush()
    time.sleep(1.0)
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"(New-Object Media.SoundPlayer '{wav}').PlaySync()"],
        check=False,
    )
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if not raw:
            continue
        if any(marker in raw for marker in KEEP):
            lines.append(raw)
            print(raw, flush=True)
        if "session finished" in raw:
            break
    time.sleep(2)
    dev.write(b"CMD:METRICS\n")
    dev.flush()
    deadline = time.monotonic() + 6
    while time.monotonic() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if "rock-gui: ui " in raw or "metrics_cast" in raw:
            lines.append(raw)
            print(raw, flush=True)
        if "rock-gui: ui " in raw:
            break


def main() -> None:
    lines: list[str] = []
    dev = serial.Serial()
    dev.port = PORT
    dev.baudrate = 115200
    dev.timeout = 0.2
    dev.dtr = False
    dev.rts = False
    dev.open()
    time.sleep(0.3)
    dev.reset_input_buffer()
    for phrase in PHRASES:
        run_phrase(dev, phrase, lines)
    dev.close()
    with open(LOG, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    print(f"saved {LOG}")


if __name__ == "__main__":
    main()
