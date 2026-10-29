#!/usr/bin/env python3
"""Add verified music negatives to a separate v6 microWakeWord feature set."""

import json
from pathlib import Path

from re8_prepare_mww import ROOT, RATE, pcm
from microwakeword.audio.audio_utils import generate_features_for_clip
from mmap_ninja.ragged import RaggedMmap

OUTPUT = ROOT / "build/re8-training/mww-v6/features/negative_music/training/audio_mmap"
SOURCES = [
    (ROOT / "build/re8-board-corpus/sessions/20261023T145710Z_ambient-radio-moderate-01_0043.wav", 0, 60),
    (ROOT / "build/re8-board-trial/last-detections/20261025T150304Z_seq0050_id0001.wav", 0, None),
    (ROOT / "build/re8-music-v6/sessions/20261026T141522Z_negative_music_external_01_0001.wav", 0, None),
]


def main() -> None:
    if OUTPUT.exists():
        raise FileExistsError(f"refusing to overwrite {OUTPUT}")
    rows = []
    features = []
    for path, start, end in SOURCES:
        if not path.is_file():
            raise FileNotFoundError(path)
        audio = pcm(path)[start * RATE : None if end is None else end * RATE]
        if len(audio) < RATE:
            raise ValueError(f"music sample is too short: {path}")
        rows.append({"path": str(path.relative_to(ROOT)), "start_s": start,
                     "end_s": end, "duration_s": len(audio) / RATE})
        features.append(generate_features_for_clip(audio, step_ms=10))
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    RaggedMmap.from_generator(OUTPUT, iter(features), batch_size=100, verbose=False)
    (OUTPUT.parent / "manifest.json").write_text(
        json.dumps(rows, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"Prepared {len(rows)} music negatives: {sum(r['duration_s'] for r in rows):.0f} s")


if __name__ == "__main__":
    main()
