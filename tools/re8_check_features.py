#!/usr/bin/env python3
"""Compare the board's fixed PCM frontend output with pymicro-features 2.0.2."""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/re8-training-env"))

from pymicro_features import MicroFrontend

from re8_collect_batch import open_serial, read_metrics
from re8_probe_board import counters, network_ready

RESULT = re.compile(r"RE8FEATURE result valid=(\d+) frames=(\d+) samples_read=(\d+) fnv1a=([0-9a-fA-F]{8})")


def reference() -> tuple[int, int, int]:
    generator = 1
    samples = bytearray()
    for _ in range(16000):
        generator = (generator * 1664525 + 1013904223) & 0xFFFFFFFF
        raw = ((generator >> 16) - 32768)
        value = (abs(raw) // 8) * (1 if raw >= 0 else -1)
        samples.extend(value.to_bytes(2, "little", signed=True))
    frontend = MicroFrontend()
    position = frames = 0
    checksum = 2166136261
    while position + 320 < len(samples):
        output = frontend.process_samples(bytes(samples[position:position + 320]))
        if not 0 < output.samples_read <= 160:
            raise RuntimeError("unexpected Python frontend sample count")
        position += output.samples_read * 2
        if output.features:
            if len(output.features) != 40:
                raise RuntimeError("unexpected Python frontend feature count")
            frames += 1
            for feature in output.features:
                raw = round(feature / 0.0390625)
                for byte in (raw & 255, (raw >> 8) & 255):
                    checksum = ((checksum ^ byte) * 16777619) & 0xFFFFFFFF
    return frames, position // 2, checksum


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True)
    args = parser.parse_args()
    expected = reference()
    print(f"PC frames={expected[0]} samples_read={expected[1]} fnv1a={expected[2]:08x}", flush=True)
    with open_serial(args.port, 115200) as device:
        network_deadline = time.monotonic() + 30
        while True:
            before = read_metrics(device, 15)
            if network_ready(before):
                break
            if time.monotonic() >= network_deadline:
                raise RuntimeError("Wi-Fi did not connect within 30 seconds")
            time.sleep(2)
        counters_before = counters(before)
        device.write(b"CMD:RE8:FEATURES\n")
        device.flush()
        deadline = time.monotonic() + 20
        found = None
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if "RE8FEATURE" in line:
                print(line, flush=True)
                match = RESULT.search(line)
                if match:
                    found = (int(match.group(2)), int(match.group(3)), int(match.group(4), 16))
                    if match.group(1) != "1":
                        raise RuntimeError("board frontend reported invalid output")
                if "RE8FEATURE end" in line:
                    break
        after = read_metrics(device, 15)
        if not network_ready(after):
            raise RuntimeError("Wi-Fi disconnected")
        counters_after = counters(after)
    for key in ("reconnects", "underruns", "overruns", "send_errors"):
        if counters_after[key] > counters_before[key]:
            raise RuntimeError(f"{key} increased during feature check")
    if found != expected:
        raise RuntimeError(f"frontend mismatch: board={found}, PC={expected}")
    print("MATCH: board and PC frontend features are identical", flush=True)


if __name__ == "__main__":
    main()
