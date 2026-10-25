#!/usr/bin/env python3
"""Run the silent RE-8 model probe with volume and network/Cast/RAM checks."""

from __future__ import annotations

import argparse
import json
import re
import time

from re8_collect_batch import open_serial, read_metrics

NETWORK = re.compile(r"\brssi=(-?\d+) ch=(\d+)\b")
COUNTER = re.compile(r"\b(reconnects|underruns|overruns|send_errors)=(\d+)\b")


def network_ready(metrics: dict[str, object]) -> bool:
    for line in metrics["lines"]:
        match = NETWORK.search(line)
        if match:
            return int(match.group(1)) > -120 and int(match.group(2)) > 0
    return False


def counters(metrics: dict[str, object]) -> dict[str, int]:
    lines = metrics["lines"]
    if not any("metrics_cast" in line for line in lines) or not any(
        "metrics_relay" in line for line in lines
    ):
        raise RuntimeError("Cast relay metrics are missing")
    return {key: int(value) for line in lines for key, value in COUNTER.findall(line)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    with open_serial(args.port, args.baud) as device:
        deadline = time.monotonic() + 30
        while True:
            before = read_metrics(device, max_volume=15)
            if network_ready(before):
                break
            if time.monotonic() >= deadline:
                raise RuntimeError("Wi-Fi did not connect within 30 seconds")
            time.sleep(2)
        device.write(b"CMD:RE8:PROBE\n")
        device.flush()
        deadline = time.monotonic() + 45
        probe: list[str] = []
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if "RE8PROBE" in line:
                probe.append(line)
                print(line, flush=True)
                if "RE8PROBE end" in line or "RE8PROBE error reason=" in line:
                    break
        if not probe or not any("RE8PROBE end" in line for line in probe):
            raise RuntimeError("RE-8 model probe did not finish")
        after = read_metrics(device, max_volume=15)
        if not network_ready(after):
            raise RuntimeError("Wi-Fi disconnected during RE-8 probe")
        before_counters, after_counters = counters(before), counters(after)
        for key in ("reconnects", "underruns", "overruns", "send_errors"):
            if key not in before_counters or key not in after_counters:
                raise RuntimeError(f"missing {key} metric")
            if after_counters[key] > before_counters[key]:
                raise RuntimeError(f"{key} increased during RE-8 probe")
    print(json.dumps({"before": before, "probe": probe, "after": after},
                     ensure_ascii=False, indent=2), flush=True)
    if not any("RE8PROBE result status=0" in line for line in probe):
        raise RuntimeError("RE-8 model inference failed")


if __name__ == "__main__":
    main()
