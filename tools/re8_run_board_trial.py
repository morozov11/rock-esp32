#!/usr/bin/env python3
"""Collect one cued ESP32-P4 session and score it on the PC after transfer.

Run with C:\Python311\python.exe. The board records; the candidate model runs
on the PC until the embedded frontend/runtime is validated separately.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from re8_score_board_session import score

ROOT = Path(__file__).resolve().parents[1]
IDF_PYTHON = Path(r"C:\Espressif\tools\python\v6.1\venv\Scripts\python.exe")
OUTPUT = ROOT / "build/re8-board-trial"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--label", required=True)
    parser.add_argument("--transcript", default="Роксервер")
    parser.add_argument("--transcripts", help="semicolon-separated phrases to rotate after cues")
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--cue-start", type=float, default=1.5)
    parser.add_argument("--distance", default="0.3m")
    parser.add_argument("--voice", default="normal")
    parser.add_argument("--playback", default="none")
    args = parser.parse_args()
    if not IDF_PYTHON.is_file():
        raise FileNotFoundError(IDF_PYTHON)
    manifest = OUTPUT / "batch_sessions.jsonl"
    previous_count = len(manifest.read_text(encoding="utf-8").splitlines()) if manifest.exists() else 0
    command = [str(IDF_PYTHON), str(ROOT / "tools/re8_collect_batch.py"),
               "--port", args.port, "--label", args.label,
               "--transcript", args.transcript, "--duration", str(args.duration),
               "--cue-start", str(args.cue_start),
               "--distance", args.distance, "--voice", args.voice,
               "--playback", args.playback, "--output-dir", str(OUTPUT)]
    command.extend(["--max-volume", "15"])
    if args.transcripts:
        command.extend(["--transcripts", args.transcripts])
    print("The board will wait two seconds, then give one cue every four seconds.", flush=True)
    subprocess.run(command, cwd=ROOT, check=True)
    rows = manifest.read_text(encoding="utf-8").splitlines()
    if len(rows) != previous_count + 1:
        raise RuntimeError("expected exactly one new verified board session")
    session = json.loads(rows[-1])
    wav = OUTPUT / session["path"]
    result = score(wav)
    result["label"] = args.label
    result["expected_phrase"] = args.transcript
    result["expected_phrases"] = args.transcripts.split(";") if args.transcripts else [args.transcript]
    result["cue_schedule"] = session["cue_schedule"]
    report = OUTPUT / "reports" / (wav.stem + ".json")
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    print(f"REPORT {report}", flush=True)


if __name__ == "__main__":
    main()
