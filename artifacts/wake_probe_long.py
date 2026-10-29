#!/usr/bin/env python3
"""Wake-path probe with a long unfiltered serial capture: arms the wake
detector, plays the command WAV, records everything, then keeps listening so
any later live attempt is captured with its server error code."""

import subprocess
import sys
import threading
import time

import serial

PORT = "COM6"
WAV = sys.argv[1] if len(sys.argv) > 1 else r"artifacts\live_probe\segments\20261025T110305Z_re8-live-commands-near_0005_cue02.wav"
LOG = sys.argv[2] if len(sys.argv) > 2 else r"artifacts\wake_probe_long_log.txt"
LISTEN_SECONDS = int(sys.argv[3]) if len(sys.argv) > 3 else 240

lines: list[str] = []
lock = threading.Lock()


def reader(dev: serial.Serial, stop_at: float) -> None:
    while time.monotonic() < stop_at:
        try:
            raw = dev.readline().decode("utf-8", errors="replace").strip()
        except Exception:
            return
        if not raw:
            continue
        stamp = time.strftime("%H:%M:%S")
        with lock:
            lines.append(f"[{stamp}] {raw}")


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

    stop_at = time.monotonic() + LISTEN_SECONDS
    thread = threading.Thread(target=reader, args=(dev, stop_at), daemon=True)
    thread.start()

    def note(text: str) -> None:
        with lock:
            lines.append(f">>> {text}")

    note(f"wake probe with {WAV}")
    dev.write(b"CMD:RE8:WAKE:ARM\n")
    dev.flush()
    time.sleep(1.5)
    subprocess.run(
        ["powershell", "-NoProfile", "-Command",
         f"(New-Object Media.SoundPlayer '{WAV}').PlaySync()"],
        check=False,
    )
    note("WAV played; listening for board events and live attempts")
    time.sleep(20)
    dev.write(b"CMD:RE8:WAKE:DISARM\n")
    dev.flush()
    note("wake disarmed after probe; continuing to listen")

    thread.join()
    dev.close()
    with open(LOG, "w", encoding="utf-8") as out:
        out.write("\n".join(lines) + "\n")
    print(f"saved {LOG} ({len(lines)} lines)")


if __name__ == "__main__":
    main()
