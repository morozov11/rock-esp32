#!/usr/bin/env python3
"""Arm RE-8 for one spoken phrase, observe RE-7, then always disarm."""

from __future__ import annotations

import argparse
import json
import time

from re8_collect_batch import open_serial, read_metrics
from re8_probe_board import network_ready
from re8_soak_board import wake


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--phrase", default="Роксервер стоп")
    args = parser.parse_args()

    with open_serial(args.port, 115200) as device:
        deadline = time.monotonic() + 45
        while True:
            before = read_metrics(device, max_volume=15)
            if network_ready(before) and wake(before)[0] == 1:
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Wi-Fi or wake task did not become ready")
            time.sleep(2)
        print(json.dumps({"before": before}, ensure_ascii=False), flush=True)
        detected = False
        finished = False
        events: list[str] = []
        try:
            device.write(b"CMD:RE8:WAKE:ARM\n")
            device.flush()
            time.sleep(2)
            print(f"ГОВОРИТЕ СЕЙЧАС: {args.phrase}", flush=True)
            deadline = time.monotonic() + 70
            while time.monotonic() < deadline:
                line = device.readline().decode("utf-8", errors="replace").strip()
                if "RE8WAKE detected" in line:
                    detected = True
                    device.write(b"CMD:RE8:WAKE:DISARM\n")
                    device.flush()
                if ("RE8WAKE" in line or "rock-voice:" in line or
                        "Guru Meditation" in line or "abort()" in line):
                    events.append(line)
                    print(line, flush=True)
                if detected and "rock-voice: session finished" in line:
                    finished = True
                    break
        finally:
            device.write(b"CMD:RE8:WAKE:DISARM\n")
            device.flush()
        after = read_metrics(device, max_volume=15)
        print(json.dumps({"after": after, "events": events}, ensure_ascii=False), flush=True)
        if not network_ready(after):
            raise RuntimeError("Wi-Fi disconnected during trial")
        if not detected:
            raise RuntimeError("wake word was not detected")
        if not finished:
            raise RuntimeError("RE-7 did not finish within trial timeout")


if __name__ == "__main__":
    main()
