#!/usr/bin/env python3
"""Collect a timed RE-8 board session and cut speech candidates by cue windows.

The ESP32-P4 retains one 5..120 second PCM16/16 kHz session in PSRAM.  This
tool plays a short PC cue at a fixed interval, writes the verified session to
disk, then creates candidate WAVs from the expected response windows.  The
schedule supplies the training label; the energy VAD only proposes cuts and
must be audited before training.
"""

from __future__ import annotations

import argparse
import base64
import json
import math
import re
import struct
import sys
import threading
import time
import wave
from datetime import datetime, timezone
from pathlib import Path

import serial

RATE_HZ = 16_000
SAMPLE_WIDTH_BYTES = 2
FRAME_MS = 20
FRAME_BYTES = RATE_HZ * SAMPLE_WIDTH_BYTES * FRAME_MS // 1000
READY = re.compile(r"RE8CAP ready id=(\d+) label=([A-Za-z0-9_-]+) delay_ms=(\d+) duration_ms=(\d+)")
BEGIN = re.compile(r"RE8CAP begin id=(\d+)")
DATA = re.compile(r"RE8CAP data id=(\d+) seq=(\d+) ([A-Za-z0-9+/=]+)")
END = re.compile(r"RE8CAP end id=(\d+) bytes=(\d+) fnv1a=([0-9a-fA-F]{8})")
ERROR = re.compile(r"RE8CAP (?:error|rejected).*")
VOLUME = re.compile(r"\bvolume=(\d+)\b")


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


def read_metrics(device: serial.Serial, max_volume: int | None) -> dict[str, object]:
    """Capture the board's network, player and RAM snapshots before/after a trial."""
    device.write(b"CMD:METRICS\n")
    device.flush()
    deadline = time.monotonic() + 6
    lines: list[str] = []
    volume: int | None = None
    while time.monotonic() < deadline:
        line = device.readline().decode("utf-8", errors="replace").strip()
        if not line:
            continue
        if any(marker in line for marker in ("rock-serial: metrics ", "rock-player: stream metrics",
                                           "rock-serial: metrics_cast", "rock-serial: metrics_relay",
                                           "rock-serial: metrics_wake",
                                           "rock-gui: ui ",
                                           "rock-serial: metrics_ram")):
            lines.append(line)
            match = VOLUME.search(line)
            if "rock-gui: ui " in line and match:
                volume = int(match.group(1))
        if "rock-serial: metrics_ram" in line:
            break
    if not any("rock-serial: metrics_ram" in line for line in lines) or volume is None:
        raise RuntimeError("board did not provide complete network/player/RAM metrics")
    if max_volume is not None and volume > max_volume:
        raise RuntimeError(f"board volume {volume}% exceeds trial limit {max_volume}%")
    return {"volume_percent": volume, "lines": lines}


def write_wav(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(SAMPLE_WIDTH_BYTES)
        output.setframerate(RATE_HZ)
        output.writeframes(data)


def pcm16_rms(data: bytes) -> int:
    """Return the RMS of little-endian signed PCM16 without external modules."""
    sample_count = len(data) // SAMPLE_WIDTH_BYTES
    if not sample_count:
        return 0
    square_sum = sum(sample * sample for (sample,) in struct.iter_unpack("<h", data))
    return math.isqrt(square_sum // sample_count)


def percentile(values: list[int], fraction: float) -> int:
    if not values:
        return 0
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(fraction * (len(ordered) - 1)))]


def cue_worker(
    started: float,
    duration: float,
    first_cue: float,
    interval: float,
    transcripts: list[str],
    cues: list[dict[str, float | int | str]],
    stop: threading.Event,
) -> None:
    cue_number = 1
    planned = first_cue
    while planned + 2.9 <= duration:
        remaining = started + planned - time.monotonic()
        if remaining > 0 and stop.wait(remaining):
            return
        if stop.is_set():
            return
        actual = time.monotonic() - started
        transcript = transcripts[(cue_number - 1) % len(transcripts)]
        try:
            if sys.platform == "win32":
                import winsound

                winsound.Beep(1400, 90)
        except RuntimeError:
            pass
        print(f"\aCUE {cue_number}: say once: {transcript}", flush=True)
        cues.append({"index": cue_number, "cue_seconds": round(actual, 3), "transcript": transcript})
        cue_number += 1
        planned += interval


def cut_candidates(
    data: bytes,
    cues: list[dict[str, float | int | str]],
    session_relative_path: str,
    session_stem: str,
    output_dir: Path,
    label: str,
    conditions: dict[str, str],
) -> list[dict[str, object]]:
    rms_values = [pcm16_rms(data[offset : offset + FRAME_BYTES])
                  for offset in range(0, len(data) - FRAME_BYTES + 1, FRAME_BYTES)]
    noise_floor = percentile(rms_values, 0.20)
    # Quiet speech needs a lower relative threshold.  The known cue window
    # constrains false candidates; transcript audit remains the final gate.
    threshold = max(160, int(noise_floor * 2.5))
    segments: list[dict[str, object]] = []
    for cue in cues:
        cue_seconds = float(cue["cue_seconds"])
        first_frame = max(0, int((cue_seconds + 0.25) * 1000 // FRAME_MS))
        last_frame = min(len(rms_values), int((cue_seconds + 2.9) * 1000 // FRAME_MS))
        active = [index for index in range(first_frame, last_frame) if rms_values[index] >= threshold]
        result: dict[str, object] = {
            "label": label,
            "transcript": cue["transcript"],
            "cue_index": cue["index"],
            "cue_seconds": cue_seconds,
            "source_session": session_relative_path,
            "conditions": conditions,
            "vad": {"frame_ms": FRAME_MS, "noise_floor_rms": noise_floor, "threshold_rms": threshold},
        }
        if len(active) < 3:
            result.update({"status": "rejected", "reason": "no_speech_energy_in_expected_window"})
            segments.append(result)
            continue
        start_seconds = max(0.0, active[0] * FRAME_MS / 1000.0 - 0.16)
        end_seconds = min(len(data) / (RATE_HZ * SAMPLE_WIDTH_BYTES), (active[-1] + 1) * FRAME_MS / 1000.0 + 0.35)
        start_byte = int(start_seconds * RATE_HZ) * SAMPLE_WIDTH_BYTES
        end_byte = int(end_seconds * RATE_HZ) * SAMPLE_WIDTH_BYTES
        clip = data[start_byte:end_byte]
        clip_rms = pcm16_rms(clip)
        clipped_samples = sum(
            1 for index in range(0, len(clip), SAMPLE_WIDTH_BYTES)
            if clip[index:index + 2] in (b"\x00\x80", b"\xff\x7f")
        )
        relative_path = f"segments/{session_stem}_cue{int(cue['index']):02d}.wav"
        write_wav(output_dir / relative_path, clip)
        result.update({
            "path": relative_path,
            "status": "candidate",
            "start_seconds": round(start_seconds, 3),
            "end_seconds": round(end_seconds, 3),
            "duration_seconds": round(end_seconds - start_seconds, 3),
            "rms": clip_rms,
            "clipped_percent": round(100.0 * clipped_samples / max(1, len(clip) // 2), 4),
        })
        segments.append(result)
    return segments


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", required=True, help="USB Serial/JTAG port, for example COM6")
    parser.add_argument("--label", required=True, help="session scenario identifier: 1..32 ASCII letters, digits, _ or -")
    parser.add_argument("--transcript", default="Роксервер", help="one phrase to say after every cue")
    parser.add_argument("--transcripts", help="semicolon-separated phrases to rotate after cues")
    parser.add_argument("--duration", type=int, default=30, choices=range(5, 121), metavar="5..120")
    parser.add_argument("--cue-start", type=float, default=1.5, help="seconds from recording start to first cue")
    parser.add_argument("--cue-interval", type=float, default=4.0, help="seconds between cues")
    parser.add_argument("--distance", default="unknown", help="distance from mouth to board, for example 0.3m")
    parser.add_argument("--voice", default="normal", help="voice level or style")
    parser.add_argument("--playback", default="none", help="background or board playback condition")
    parser.add_argument("--max-volume", type=int, help="reject trial if board volume exceeds this percent")
    parser.add_argument("--output-dir", type=Path, default=Path("build/re8-board-corpus"))
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--timeout", type=float, default=None, help="total serial timeout; default is recording duration plus 240 s")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_-]{1,32}", args.label):
        parser.error("--label accepts 1..32 letters, digits, _ or -")
    if args.cue_start < 0 or args.cue_interval < 3.0:
        parser.error("--cue-start must be non-negative and --cue-interval must be at least 3 seconds")
    transcripts = ([item.strip() for item in args.transcripts.split(";")]
                   if args.transcripts else [args.transcript])
    if not transcripts or any(not item for item in transcripts):
        parser.error("--transcripts must contain one or more non-empty phrases")
    if args.max_volume is not None and not 0 <= args.max_volume <= 100:
        parser.error("--max-volume must be between 0 and 100")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    started_at = datetime.now(timezone.utc)
    deadline = time.monotonic() + (args.timeout if args.timeout is not None else args.duration + 240.0)
    chunks: dict[int, bytes] = {}
    capture_id: int | None = None
    expected_bytes: int | None = None
    cue_stop = threading.Event()
    cue_thread: threading.Thread | None = None
    cues: list[dict[str, float | int | str]] = []

    try:
        with open_serial(args.port, args.baud) as device:
            metrics_before = read_metrics(device, args.max_volume)
            print(f"BOARD METRICS before: volume={metrics_before['volume_percent']}%", flush=True)
            device.write(f"CMD:RE8:BULK:{args.label}:{args.duration}\n".encode("ascii"))
            device.flush()
            print(f"Armed {args.label}; {args.duration}s session, rotate these phrases after PC cues: {transcripts}", flush=True)
            while time.monotonic() < deadline:
                line = device.readline().decode("utf-8", errors="replace").strip()
                if not line:
                    continue
                if ERROR.search(line):
                    raise RuntimeError(line)
                if match := READY.search(line):
                    capture_id = int(match.group(1))
                    expected_bytes = RATE_HZ * int(match.group(4)) // 1000 * SAMPLE_WIDTH_BYTES
                    print(f"READY: board waits {match.group(3)} ms, then records {match.group(4)} ms", flush=True)
                    continue
                if match := BEGIN.search(line):
                    if capture_id == int(match.group(1)):
                        recording_started = time.monotonic()
                        cue_thread = threading.Thread(
                            target=cue_worker,
                            args=(recording_started, args.duration, args.cue_start, args.cue_interval,
                                  transcripts, cues, cue_stop),
                            name="re8-cues",
                            daemon=True,
                        )
                        cue_thread.start()
                        print("RECORDING: wait for each cue, then speak once.", flush=True)
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
                    if len(data) != expected_bytes:
                        raise RuntimeError(f"incomplete capture: got {len(data)}, expected {expected_bytes}")
                    if fnv1a(data) != expected_hash:
                        raise RuntimeError("checksum mismatch")
                    metrics_after = read_metrics(device, args.max_volume)
                    cue_stop.set()
                    if cue_thread:
                        cue_thread.join(timeout=1.0)
                    stamp = started_at.strftime("%Y%m%dT%H%M%SZ")
                    stem = f"{stamp}_{args.label}_{capture_id:04d}"
                    session_relative_path = f"sessions/{stem}.wav"
                    write_wav(args.output_dir / session_relative_path, data)
                    conditions = {"distance": args.distance, "voice": args.voice, "playback": args.playback}
                    session = {
                        "path": session_relative_path,
                        "label": args.label,
                        "captured_at": started_at.isoformat(),
                        "capture_id": capture_id,
                        "sample_rate_hz": RATE_HZ,
                        "channels": 1,
                        "sample_width_bits": 16,
                        "duration_seconds": args.duration,
                        "source": "rock-esp32-es8311",
                        "conditions": conditions,
                        "cue_schedule": cues,
                        "metrics_before": metrics_before,
                        "metrics_after": metrics_after,
                    }
                    segments = cut_candidates(data, cues, session_relative_path, stem, args.output_dir,
                                              args.label, conditions)
                    with (args.output_dir / "batch_sessions.jsonl").open("a", encoding="utf-8") as manifest:
                        manifest.write(json.dumps(session, ensure_ascii=False) + "\n")
                    with (args.output_dir / "batch_segments.jsonl").open("a", encoding="utf-8") as manifest:
                        for segment in segments:
                            manifest.write(json.dumps(segment, ensure_ascii=False) + "\n")
                    candidates = sum(segment["status"] == "candidate" for segment in segments)
                    print(f"SAVED {args.output_dir / session_relative_path}; {candidates}/{len(segments)} speech candidates", flush=True)
                    return 0
    finally:
        cue_stop.set()
        if cue_thread:
            cue_thread.join(timeout=1.0)
    raise TimeoutError("board did not complete a batch before --timeout")


if __name__ == "__main__":
    raise SystemExit(main())
