#!/usr/bin/env python3
"""Receive one RE-8 training clip from the ESP32-P4 over USB Serial/JTAG.

The firmware command starts a six-second countdown, captures four seconds on
the board microphone, and sends the completed PCM16 payload as framed base64.
This tool validates its checksum, writes a 16 kHz mono WAV, and appends one
metadata row to JSONL. It deliberately never forwards audio to RockServer.
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
READY = re.compile(r"RE8CAP ready id=(\d+) label=([A-Za-z0-9_-]+) delay_ms=(\d+) duration_ms=(\d+)")
BEGIN = re.compile(r"RE8CAP begin id=(\d+)")
DATA = re.compile(r"RE8CAP data id=(\d+) seq=(\d+) ([A-Za-z0-9+/=]+)")
END = re.compile(r"RE8CAP end id=(\d+) bytes=(\d+) fnv1a=([0-9a-fA-F]{8})")
ERROR = re.compile(r"RE8CAP (?:error|rejected).*")


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
    parser.add_argument("--label", required=True, help="positive, confuser or ambient scenario identifier")
    parser.add_argument("--output-dir", type=Path, default=Path("build/re8-board-corpus"))
    parser.add_argument("--distance", default="unknown")
    parser.add_argument("--voice", default="normal")
    parser.add_argument("--playback", default="none")
    parser.add_argument("--transcript", default="Роксервер")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=50.0)
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", args.label):
        parser.error("--label accepts 1..32 letters, digits, _ or -")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    chunks: dict[int, bytes] = {}
    capture_id: int | None = None
    expected_bytes: int | None = None
    started_at = datetime.now(timezone.utc)
    deadline = time.monotonic() + args.timeout

    with open_serial(args.port, args.baud) as device:
        device.write(f"CMD:RE8:CAPTURE:{args.label}\n".encode("ascii"))
        device.flush()
        print(f"Armed {args.label}; wait for READY, then say: {args.transcript}", flush=True)
        while time.monotonic() < deadline:
            line = device.readline().decode("utf-8", errors="replace").strip()
            if not line:
                continue
            if ERROR.search(line):
                raise RuntimeError(line)
            if match := READY.search(line):
                capture_id = int(match.group(1))
                expected_bytes = RATE_HZ * int(match.group(4)) // 1000 * SAMPLE_WIDTH_BYTES
                print(f"READY: speak after {match.group(3)} ms; recording lasts {match.group(4)} ms", flush=True)
                continue
            if match := BEGIN.search(line):
                if capture_id == int(match.group(1)):
                    print("RECORDING", flush=True)
                continue
            if match := DATA.search(line):
                if capture_id != int(match.group(1)):
                    continue
                sequence = int(match.group(2))
                if sequence in chunks:
                    raise RuntimeError(f"duplicate data sequence {sequence}")
                chunks[sequence] = base64.b64decode(match.group(3), validate=True)
                continue
            if match := END.search(line):
                if capture_id != int(match.group(1)):
                    continue
                expected_bytes = int(match.group(2))
                expected_hash = int(match.group(3), 16)
                data = b"".join(chunks[index] for index in range(len(chunks)))
                if expected_bytes is None or len(data) != expected_bytes:
                    raise RuntimeError(f"incomplete capture: got {len(data)}, expected {expected_bytes}")
                if fnv1a(data) != expected_hash:
                    raise RuntimeError("checksum mismatch")
                stamp = started_at.strftime("%Y%m%dT%H%M%SZ")
                filename = f"{stamp}_{args.label}_{capture_id:04d}.wav"
                wav_path = args.output_dir / filename
                with wave.open(str(wav_path), "wb") as output:
                    output.setnchannels(1)
                    output.setsampwidth(SAMPLE_WIDTH_BYTES)
                    output.setframerate(RATE_HZ)
                    output.writeframes(data)
                metadata = {
                    "path": filename,
                    "label": args.label,
                    "transcript": args.transcript,
                    "captured_at": started_at.isoformat(),
                    "sample_rate_hz": RATE_HZ,
                    "channels": 1,
                    "sample_width_bits": 16,
                    "distance": args.distance,
                    "voice": args.voice,
                    "playback": args.playback,
                    "source": "rock-esp32-es8311",
                    "capture_id": capture_id,
                }
                with (args.output_dir / "manifest.jsonl").open("a", encoding="utf-8") as manifest:
                    manifest.write(json.dumps(metadata, ensure_ascii=False) + "\n")
                print(f"SAVED {wav_path} ({len(data)} bytes, fnv1a={expected_hash:08x})")
                return 0
    raise TimeoutError("board did not complete a capture before --timeout")


if __name__ == "__main__":
    raise SystemExit(main())
