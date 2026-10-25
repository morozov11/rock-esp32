#!/usr/bin/env python3
"""Fetch the last RE-8 wake threshold-crossing snapshot from the ESP32-P4.

The firmware keeps the two-second pre-roll PCM of the most recent crossing
(armed or disarmed) in PSRAM. CMD:RE8:WAKE:LAST streams it as framed base64
with an FNV-1a checksum; this tool validates the checksum and writes a
16 kHz mono WAV plus a JSON sidecar for false-positive review. It never
forwards audio to RockServer and never opens the microphone.
"""

from __future__ import annotations

import argparse
import base64
import json
import re
import time
import wave
from datetime import datetime, timezone
from pathlib import Path

import serial

RATE_HZ = 16_000
SAMPLE_WIDTH_BYTES = 2
BEGIN = re.compile(r"RE8WAKE dump begin id=(\d+) seq=(\d+) bytes=(\d+)")
DATA = re.compile(r"RE8WAKE dump data id=(\d+) seq=(\d+) ([A-Za-z0-9+/=]+)")
END = re.compile(r"RE8WAKE dump end id=(\d+) seq=(\d+) bytes=(\d+) fnv1a=([0-9a-fA-F]{8})")
FAILURE = re.compile(r"RE8WAKE dump (?:error|rejected).*")


def fnv1a(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def open_serial(port: str, baud: int) -> serial.Serial:
    device = serial.Serial()
    device.port = port
    device.baudrate = baud
    device.timeout = 0.25
    # USB Serial/JTAG can reset the P4 when DTR/RTS are asserted on open.
    device.dtr = False
    device.rts = False
    device.open()
    return device


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB Serial/JTAG port, for example COM6")
    parser.add_argument("--output-dir", type=Path, default=Path("build/re8-board-trial/last-detections"))
    parser.add_argument("--note", default="", help="free-form context saved into the sidecar")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    chunks: dict[int, bytes] = {}
    dump_id: int | None = None
    snapshot_seq: int | None = None
    started_at = datetime.now(timezone.utc)
    deadline = time.monotonic() + args.timeout

    with open_serial(args.port, args.baud) as device:
        device.write(b"CMD:RE8:WAKE:LAST\n")
        device.flush()
        completed = False
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if FAILURE.search(line):
                raise RuntimeError(line)
            if match := BEGIN.search(line):
                dump_id = int(match.group(1))
                snapshot_seq = int(match.group(2))
                expected_begin_bytes = int(match.group(3))
                print(f"dump begin id={dump_id} seq={snapshot_seq} bytes={expected_begin_bytes}",
                      flush=True)
                continue
            if match := DATA.search(line):
                if dump_id != int(match.group(1)):
                    continue
                sequence = int(match.group(2))
                if sequence in chunks:
                    raise RuntimeError(f"duplicate data sequence {sequence}")
                chunks[sequence] = base64.b64decode(match.group(3), validate=True)
                continue
            if match := END.search(line):
                if dump_id != int(match.group(1)):
                    continue
                expected_bytes = int(match.group(3))
                expected_hash = int(match.group(4), 16)
                completed = True
                break
    if not completed:
        raise TimeoutError("board did not complete a dump before --timeout")

    data = b"".join(chunks[index] for index in range(len(chunks)))
    if len(data) != expected_bytes or len(data) != expected_begin_bytes:
        raise RuntimeError(f"incomplete dump: got {len(data)}, expected {expected_bytes}")
    if fnv1a(data) != expected_hash:
        raise RuntimeError("checksum mismatch")

    stamp = started_at.strftime("%Y%m%dT%H%M%SZ")
    wav_path = args.output_dir / f"{stamp}_seq{snapshot_seq:04d}_id{dump_id:04d}.wav"
    with wave.open(str(wav_path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(SAMPLE_WIDTH_BYTES)
        output.setframerate(RATE_HZ)
        output.writeframes(data)
    sidecar = {
        "path": wav_path.name,
        "snapshot_seq": snapshot_seq,
        "dump_id": dump_id,
        "captured_at": started_at.isoformat(),
        "sample_rate_hz": RATE_HZ,
        "channels": 1,
        "sample_width_bits": 16,
        "duration_s": len(data) / (RATE_HZ * SAMPLE_WIDTH_BYTES),
        "fnv1a": f"{expected_hash:08x}",
        "note": args.note,
        "source": "rock-wake-preroll-snapshot",
    }
    sidecar_path = wav_path.with_suffix(".json")
    sidecar_path.write_text(json.dumps(sidecar, ensure_ascii=False, indent=2) + "\n",
                            encoding="utf-8")
    print(f"SAVED {wav_path} ({len(data)} bytes, fnv1a={expected_hash:08x})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
