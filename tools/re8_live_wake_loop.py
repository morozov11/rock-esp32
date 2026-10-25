#!/usr/bin/env python3
"""Cycle spoken RE-8 commands with PC-speaker cues, one phrase per beep.

Arms the on-device wake detector, beeps from the computer speakers, waits
for one spoken phrase, observes the RE-7 voice session, disarms, then moves
to the next phrase.  Optionally blocks until a "go" file appears so the
operator can confirm the start in chat before any cue is given.
"""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import time
from pathlib import Path

from re8_collect_batch import open_serial, read_metrics
from re8_probe_board import network_ready
from re8_soak_board import wake

PHRASES = ["Роксервер стоп", "Роксервер старт", "Роксервер громче", "Роксервер тише"]
FINISHED = re.compile(r"rock-voice: session finished state=(\d+)")


def beep(times: int = 2) -> None:
    # [console]::beep plays through the sound card, so the cue is audible
    # even when this script's stdout is captured by the agent's shell.
    for _ in range(times):
        subprocess.run(
            ["powershell", "-NoProfile", "-Command", "[console]::beep(880,250)"],
            check=False,
        )
        time.sleep(0.15)


def wait_ready(device, timeout_s: float = 45.0) -> None:
    deadline = time.monotonic() + timeout_s
    while True:
        metrics = read_metrics(device, max_volume=15)
        if network_ready(metrics) and wake(metrics)[0] == 1:
            return
        if time.monotonic() >= deadline:
            raise RuntimeError("Wi-Fi or wake task did not become ready")
        time.sleep(2)


def one_phrase(device, phrase: str) -> dict:
    result = {"phrase": phrase, "detected": False, "gated": 0,
              "session_state": None, "events": []}
    device.write(b"CMD:RE8:WAKE:ARM\n")
    device.flush()
    time.sleep(2)
    beep()
    print(f"ГОВОРИТЕ СЕЙЧАС: {phrase}", flush=True)
    deadline = time.monotonic() + 70
    try:
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if "RE8WAKE gated" in line:
                result["gated"] += 1
            if "RE8WAKE crossing" in line:
                result["detected"] = True
                device.write(b"CMD:RE8:WAKE:DISARM\n")
                device.flush()
            if ("RE8WAKE" in line or "rock-voice:" in line or
                    "Guru Meditation" in line or "abort()" in line):
                result["events"].append(line)
                print(line, flush=True)
            match = FINISHED.search(line)
            if match:
                result["session_state"] = int(match.group(1))
                break
    finally:
        device.write(b"CMD:RE8:WAKE:DISARM\n")
        device.flush()
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--gap", type=float, default=5.0,
                        help="seconds between phrases")
    parser.add_argument("--go-file", type=Path,
                        help="block until this file exists (chat confirmation)")
    args = parser.parse_args()

    if args.go_file:
        print(f"WAITING FOR GO FILE: {args.go_file}", flush=True)
        while not args.go_file.exists():
            time.sleep(0.5)
        print("GO", flush=True)

    results = []
    with open_serial(args.port, 115200) as device:
        wait_ready(device)
        print("READY", flush=True)
        for round_index in range(args.rounds):
            for phrase in PHRASES:
                if round_index or phrase != PHRASES[0]:
                    time.sleep(args.gap)
                print(json.dumps({"round": round_index + 1}, ensure_ascii=False),
                      flush=True)
                results.append(one_phrase(device, phrase))
                print(json.dumps(results[-1], ensure_ascii=False), flush=True)
        after = read_metrics(device, max_volume=15)
    summary = {phrase: {
        "trials": sum(1 for r in results if r["phrase"] == phrase),
        "detected": sum(1 for r in results if r["phrase"] == phrase and r["detected"]),
        "gated": sum(r["gated"] for r in results if r["phrase"] == phrase),
    } for phrase in PHRASES}
    print(json.dumps({"summary": summary, "after": after}, ensure_ascii=False),
          flush=True)


if __name__ == "__main__":
    main()
