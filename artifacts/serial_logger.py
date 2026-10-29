#!/usr/bin/env python3
"""Combined long serial monitor + prober: keeps the wake detector armed so
live spoken attempts are recorded; sends periodic CMD:METRICS snapshots; and
runs one controlled CMD:UI:VOICE probe with a WAV played over the PC speakers
while Cast playback is active, to reproduce the reported failure."""

import subprocess
import sys
import threading
import time

import serial

PORT = "COM6"
LOG = sys.argv[1] if len(sys.argv) > 1 else r"artifacts\serial_long_log.txt"
SECONDS = int(sys.argv[2]) if len(sys.argv) > 2 else 900
WAV = r"artifacts\live_probe\segments\20261025T110305Z_re8-live-commands-near_0005_cue02.wav"
PROBE_AT = 45  # seconds into the capture

lines: list[str] = []
lock = threading.Lock()


def note(text: str) -> None:
    with lock:
        lines.append(f"[{time.strftime('%H:%M:%S')}] >>> {text}")


def main() -> None:
    dev = serial.Serial()
    dev.port = PORT
    dev.baudrate = 115200
    dev.timeout = 0.2
    dev.dtr = False
    dev.rts = False
    dev.open()
    time.sleep(0.3)
    dev.reset_input_buffer()
    dev.write(b"CMD:RE8:WAKE:ARM\n")
    dev.flush()
    note("capture started; wake ARMED; speak commands any time")

    started = time.monotonic()
    stop_at = started + SECONDS
    probed = False
    next_metrics = started + 10

    def reader() -> None:
        while time.monotonic() < stop_at:
            try:
                raw = dev.readline().decode("utf-8", errors="replace").strip()
            except Exception:
                return
            if not raw:
                continue
            with lock:
                lines.append(f"[{time.strftime('%H:%M:%S')}] {raw}")

    thread = threading.Thread(target=reader, daemon=True)
    thread.start()

    while time.monotonic() < stop_at:
        now = time.monotonic()
        if not probed and now - started >= PROBE_AT:
            probed = True
            note("controlled probe: CMD:UI:VOICE + WAV over speakers")
            dev.write(b"CMD:UI:VOICE\n")
            dev.flush()
            time.sleep(1.0)
            subprocess.run(
                ["powershell", "-NoProfile", "-Command",
                 f"(New-Object Media.SoundPlayer '{WAV}').PlaySync()"],
                check=False,
            )
            note("WAV played")
        elif now >= next_metrics:
            next_metrics = now + 120
            dev.write(b"CMD:METRICS\n")
            dev.flush()
        time.sleep(0.5)

    thread.join()
    dev.close()
    with open(LOG, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    print(f"saved {LOG} ({len(lines)} lines)")


if __name__ == "__main__":
    main()
