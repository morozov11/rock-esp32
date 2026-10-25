#!/usr/bin/env python3
"""Watch the always-on wake task in silence and compare device metrics."""

from __future__ import annotations

import argparse
import json
import re
import time

from re8_collect_batch import open_serial, read_metrics
from re8_probe_board import counters, network_ready

WAKE = re.compile(r"metrics_wake running=(\d+) frames=(\d+) invokes=(\d+) detections=(\d+)")


def wake(metrics: dict[str, object]) -> tuple[int, int, int, int]:
    for line in metrics["lines"]:
        match = WAKE.search(line)
        if match:
            return tuple(map(int, match.groups()))
    raise RuntimeError("wake metrics missing")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--arm", action="store_true", help="arm for this trial, then disarm")
    args = parser.parse_args()
    if args.seconds < 10:
        parser.error("--seconds must be at least 10")

    with open_serial(args.port, 115200) as device:
        connect_deadline = time.monotonic() + 45
        while True:
            before = read_metrics(device, max_volume=15)
            if network_ready(before) and wake(before)[0] == 1:
                break
            if time.monotonic() >= connect_deadline:
                raise RuntimeError("Wi-Fi or wake task did not become ready")
            time.sleep(2)
        print(json.dumps({"before": before}, ensure_ascii=False), flush=True)
        deadline = time.monotonic() + args.seconds
        wake_lines: list[str] = []
        if args.arm:
            device.write(b"CMD:RE8:WAKE:ARM\n")
            device.flush()
        try:
            while time.monotonic() < deadline:
                line = device.readline().decode("utf-8", errors="replace").strip()
                if "RE8WAKE" in line or "Guru Meditation" in line or "abort()" in line:
                    wake_lines.append(line)
                    print(line, flush=True)
                    if "RE8WAKE detected" in line:
                        break
        finally:
            if args.arm:
                device.write(b"CMD:RE8:WAKE:DISARM\n")
                device.flush()
        after = read_metrics(device, max_volume=15)
        print(json.dumps({"after": after, "wake_lines": wake_lines}, ensure_ascii=False), flush=True)
        if not network_ready(after):
            raise RuntimeError("Wi-Fi disconnected during soak")
        first, last = counters(before), counters(after)
        wake_before, wake_after = wake(before), wake(after)
        if wake_before[0] != 1 or wake_after[0] != 1:
            raise RuntimeError("wake task is not running")
        if wake_after[1] <= wake_before[1] or wake_after[2] <= wake_before[2]:
            raise RuntimeError("wake frontend or inference did not advance")
        if wake_after[3] != wake_before[3]:
            raise RuntimeError("wake detection count increased during silence")
        for key in ("reconnects", "underruns", "overruns", "send_errors"):
            if key not in first or key not in last or last[key] > first[key]:
                raise RuntimeError(f"{key} increased or is missing")
        if any("RE8WAKE stopped" in line for line in wake_lines):
            raise RuntimeError("wake task stopped")
        if any("RE8WAKE detected" in line for line in wake_lines):
            raise RuntimeError("wake triggered during silence")


if __name__ == "__main__":
    main()
