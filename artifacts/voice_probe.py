#!/usr/bin/env python3
"""One controlled voice probe: trigger a voice session over serial, play a
recorded command WAV through the PC speakers, and capture the full serial log
including the server error code, transcript and session end state."""

import subprocess
import sys
import time

import serial

PORT = "COM6"
WAV = sys.argv[1] if len(sys.argv) > 1 else r"artifacts\live_probe\segments\20261025T110305Z_re8-live-commands-near_0005_cue02.wav"
LOG = sys.argv[2] if len(sys.argv) > 2 else r"artifacts\voice_probe_log.txt"

KEEP = ("rock-voice", "RE8WAKE", "rock-gui", "rock-control", "Guru Meditation",
        "abort()", "rock-serial: metrics", "rock-player: stream metrics")


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

    dev.write(b"CMD:UI:VOICE\n")
    dev.flush()
    lines.append(">>> sent CMD:UI:VOICE")
    time.sleep(1.0)  # session connects before the phrase starts

    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"(New-Object Media.SoundPlayer '{WAV}').PlaySync()"],
        check=False,
    )
    lines.append(f">>> played {WAV}")

    deadline = time.monotonic() + 70
    finished = False
    while time.monotonic() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if not raw:
            continue
        if any(marker in raw for marker in KEEP):
            lines.append(raw)
            print(raw, flush=True)
        if "session finished" in raw:
            finished = True
            break
    if not finished:
        lines.append(">>> no session finished line within 70s")

    dev.write(b"CMD:METRICS\n")
    dev.flush()
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        raw = dev.readline().decode("utf-8", errors="replace").strip()
        if "metrics_ram" in raw:
            lines.append(raw)
            print(raw, flush=True)
            break

    dev.close()
    with open(LOG, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    print(f"saved {LOG}")


if __name__ == "__main__":
    main()
