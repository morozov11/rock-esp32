#!/usr/bin/env python3
"""Audit RE-8 candidate cuts with local Whisper; preserve schedule labels.

Run with PYTHONPATH=build/re8-audit-env. Results append to a JSONL file and
are resumable. Whisper is an independent check, not the ground-truth label.
"""

from __future__ import annotations

import argparse
import json
import re
import unicodedata
import wave
from difflib import SequenceMatcher
from pathlib import Path

import numpy as np
import whisper


def normalized(text: str) -> str:
    text = unicodedata.normalize("NFKC", text).lower().replace("ё", "е")
    return " ".join(re.findall(r"[а-яa-z0-9]+", text))


def load_pcm(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth(), wav.getframerate()) != (1, 2, 16000):
            raise ValueError(f"expected PCM16 16 kHz mono: {path}")
        return np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").astype(np.float32) / 32768.0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--corpus", type=Path, default=Path("build/re8-board-corpus"))
    parser.add_argument("--model", default="base", help="multilingual Whisper model")
    parser.add_argument("--model-dir", type=Path, default=Path("build/re8-audit-models"))
    parser.add_argument("--output", type=Path, default=Path("build/re8-board-corpus/whisper_audit.jsonl"))
    parser.add_argument("--group", choices=("all", "wake", "command", "confuser"), default="all")
    parser.add_argument("--limit", type=int, default=0, help="maximum new clips; zero means all")
    args = parser.parse_args()
    args.model_dir.mkdir(parents=True, exist_ok=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    rows = [json.loads(line) for line in (args.corpus / "batch_segments.jsonl").open(encoding="utf-8")]
    rows = [row for row in rows if row.get("status") == "candidate"]
    if args.group != "all":
        prefix = {"wake": "p", "command": "cmd-", "confuser": "confuser-"}[args.group]
        rows = [row for row in rows if row["label"].startswith(prefix)]
    completed = set()
    if args.output.exists():
        completed = {json.loads(line)["path"] for line in args.output.open(encoding="utf-8") if line.strip()}
    pending = [row for row in rows if row["path"] not in completed]
    if args.limit:
        pending = pending[: args.limit]
    if not pending:
        print("No pending clips", flush=True)
        return
    print(f"Loading {args.model}; auditing {len(pending)} clips", flush=True)
    model = whisper.load_model(args.model, device="cpu", download_root=str(args.model_dir))
    with args.output.open("a", encoding="utf-8") as output:
        for index, row in enumerate(pending, 1):
            result = model.transcribe(
                load_pcm(args.corpus / row["path"]),
                language="ru",
                task="transcribe",
                fp16=False,
                temperature=0,
                condition_on_previous_text=False,
            )
            expected = normalized(row["transcript"])
            heard = normalized(result["text"])
            audit = {
                "path": row["path"],
                "label": row["label"],
                "expected": row["transcript"],
                "heard": result["text"].strip(),
                "similarity": round(SequenceMatcher(None, expected, heard).ratio(), 3),
                "model": args.model,
                "review": "pending_human",
            }
            output.write(json.dumps(audit, ensure_ascii=False) + "\n")
            output.flush()
            print(f"{index}/{len(pending)} {row['label']}: {audit['heard']!r} (similarity {audit['similarity']})", flush=True)


if __name__ == "__main__":
    main()
