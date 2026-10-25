#!/usr/bin/env python3
"""Prepare reviewed RE-8 audio for the upstream microWakeWord trainer.

Run with C:\Python311\python.exe. Outputs stay under build/re8-training/mww-v5.
"""

from __future__ import annotations

import argparse
import csv
import json
import random
import shutil
import sys
import wave
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "build/re8-training-env"), str(ROOT / "build/re8-training-source")]

import numpy as np
from mmap_ninja.ragged import RaggedMmap
from microwakeword.audio.audio_utils import generate_features_for_clip
from scipy.signal import resample_poly

RATE = 16000
WINDOW = int(3.2 * RATE)
OUTPUT = ROOT / "build/re8-training/mww-v5"


def pcm(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wav:
        if (wav.getnchannels(), wav.getsampwidth()) != (1, 2):
            raise ValueError(f"expected mono PCM16: {path}")
        audio = np.frombuffer(wav.readframes(wav.getnframes()), dtype="<i2").copy()
        if wav.getframerate() == 22050 and "re8-probe" in path.parts:
            return np.clip(np.rint(resample_poly(audio, 320, 441)), -32768, 32767).astype(np.int16)
        if wav.getframerate() != RATE:
            raise ValueError(f"expected 16 kHz (or probe 22.05 kHz): {path}")
        return audio


def entries() -> list[dict]:
    reviewed = list(csv.DictReader((ROOT / "artifacts/re8-review-queue-20261025.csv").open(encoding="utf-8-sig", newline="")))
    assert len(reviewed) == 225 and len({r["path"] for r in reviewed}) == 225
    assert Counter(r["decision"] for r in reviewed) == {"accept": 214, "reject": 11}
    out = []
    for row in reviewed:
        if row["decision"] != "accept":
            continue
        label = row["label"]
        if label.startswith("cmd-"):
            if label.endswith("-01"):
                kind, split = "positive_command", "training"
            else:
                kind, split = "command_eval", "testing"
        elif label.startswith("confuser-"):
            kind = "negative_real"
            cue = int(row["path"].rsplit("_cue", 1)[1][:2])
            split = "training" if cue <= 3 else "validation" if cue == 4 else "testing"
        else:
            kind = "positive_real"
            split = "validation" if "-batch-02" in label else "testing" if "-batch-03" in label else "training"
        out.append({"path": str(ROOT / "build/re8-board-corpus" / row["path"]), "kind": kind, "split": split, "origin": label})

    for row in json.loads((ROOT / "build/re8-corpus/manifest.json").read_text(encoding="utf-8")):
        voice = row["source_file"].split("_")[0]
        out.append({
            "path": str(ROOT / "build/re8-corpus" / row["path"]),
            "kind": "positive_synth",
            "split": "testing" if voice == "ruslan" else "training",
            "origin": "piper:" + row["source_file"],
            "trim_pre_ms": row["parameters"]["pre_ms"],
            "trim_post_ms": row["parameters"]["post_ms"],
        })

    for row in json.loads((ROOT / "build/re8-probe/manifest.json").read_text(encoding="utf-8")):
        if row["label"] != "negative":
            continue
        if row["voice"] != "ruslan":
            continue
        out.append({"path": str(ROOT / "build/re8-probe/audio" / Path(row["path"]).name), "kind": "negative_synth", "split": "testing", "origin": "piper:" + row["voice"] + ":" + row["phrase"]})

    for row in json.loads((ROOT / "build/re8-negative-corpus-v4/manifest.json").read_text(encoding="utf-8")):
        out.append({"path": str(ROOT / "build/re8-negative-corpus-v4" / row["path"]), "kind": "negative_synth", "split": "training",
                    "origin": "piper:" + row["source_file"], "trim_pre_ms": row["parameters"]["pre_ms"],
                    "trim_post_ms": row["parameters"]["post_ms"]})

    for row in map(json.loads, (ROOT / "build/re8-board-corpus/batch_sessions.jsonl").open(encoding="utf-8")):
        if not row["label"].startswith("ambient-"):
            continue
        for split, begin, end in (("training", 0, 60), ("validation_ambient", 60, 90), ("testing_ambient", 90, 120)):
            out.append({"path": str(ROOT / "build/re8-board-corpus" / row["path"]), "kind": "ambient", "split": split, "origin": row["label"], "begin": begin, "end": end})

    assert all(Path(row["path"]).is_file() for row in out)
    return out


def feature(row: dict, training_ambient: list[np.ndarray]) -> np.ndarray:
    audio = pcm(Path(row["path"]))
    if row["kind"] == "ambient":
        audio = audio[row["begin"] * RATE : row["end"] * RATE]
    else:
        if "trim_pre_ms" in row:
            audio = audio[max(0, (row["trim_pre_ms"] - 100) * RATE // 1000) :]
            audio = audio[: max(1, len(audio) - (row["trim_post_ms"] - 100) * RATE // 1000)]
        if row["kind"] == "positive_command":
            audio = audio[: int(1.6 * RATE)]  # keep the wake word at the front of the command
        audio = audio[-WINDOW:]
        window = np.zeros(WINDOW, dtype=np.int16)
        post = min(3200, WINDOW - len(audio))
        start = WINDOW - post - len(audio)
        window[start : start + len(audio)] = audio
        audio = window
        if row["origin"].startswith("piper:") and row["split"] == "training" and training_ambient and random.random() < 0.5:
            noise = random.choice(training_ambient)
            offset = random.randrange(0, len(noise) - WINDOW + 1)
            noise = noise[offset : offset + WINDOW].astype(np.float32)
            signal = audio.astype(np.float32)
            signal_rms = np.sqrt(np.mean(signal * signal))
            noise_rms = np.sqrt(np.mean(noise * noise))
            if signal_rms > 0 and noise_rms > 0:
                snr_db = random.uniform(5, 20)
                audio = np.clip(signal + noise * signal_rms / noise_rms / (10 ** (snr_db / 20)), -32768, 32767).astype(np.int16)
    result = generate_features_for_clip(audio, step_ms=10)
    assert result.ndim == 2 and result.shape[1] == 40 and result.shape[0] >= 302
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    rows = entries()
    counts = Counter((r["kind"], r["split"]) for r in rows)
    print("Input counts:", dict(sorted(counts.items())), flush=True)
    ambient_train = [pcm(Path(r["path"]))[: 60 * RATE] for r in rows if r["kind"] == "ambient" and r["split"] == "training"]
    if args.check_only:
        for kind in ("positive_real", "positive_synth", "positive_command", "negative_real", "negative_synth", "ambient"):
            row = next(r for r in rows if r["kind"] == kind)
            print(kind, feature(row, ambient_train).shape, flush=True)
        return

    OUTPUT.mkdir(parents=True, exist_ok=True)
    random.seed(8)
    manifest = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in rows)
    manifest_path = OUTPUT / "manifest.jsonl"
    if manifest_path.exists() and manifest_path.read_text(encoding="utf-8") != manifest:
        raise ValueError("existing manifest differs; use a new output directory")
    manifest_path.write_text(manifest, encoding="utf-8")
    for kind in ("positive_real", "positive_synth", "positive_command", "negative_real", "negative_synth", "ambient"):
        for split in ("training", "validation", "testing", "validation_ambient", "testing_ambient"):
            selected = [r for r in rows if r["kind"] == kind and r["split"] == split]
            if not selected:
                continue
            path = OUTPUT / "features" / kind / split / "audio_mmap"
            path.parent.mkdir(parents=True, exist_ok=True)
            if (path / "shapes_are_flat.ninja").is_file():
                print(f"Already complete: {kind}/{split}", flush=True)
                continue
            if path.exists():
                if not path.resolve().is_relative_to(OUTPUT.resolve()):
                    raise ValueError(f"unsafe output path: {path}")
                shutil.rmtree(path)
            print(f"Generating {kind}/{split}: {len(selected)}", flush=True)
            RaggedMmap.from_generator(path, (feature(r, ambient_train) for r in selected), batch_size=100, verbose=False)
    print("Feature preparation complete", flush=True)


if __name__ == "__main__":
    main()
