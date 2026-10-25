#!/usr/bin/env python3
"""Augment existing Piper confuser seeds for RE-8 microWakeWord training."""

from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "build/re8-training-env"))

import numpy as np

from re8_generate_synthetic_corpus import augment, read_pcm16, write_pcm16

OUTPUT = ROOT / "build/re8-negative-corpus-v4"


def main() -> None:
    if OUTPUT.exists():
        raise FileExistsError(OUTPUT)
    rows = json.loads((ROOT / "build/re8-probe/manifest.json").read_text(encoding="utf-8"))
    seeds = [r for r in rows if r["label"] == "negative" and r["voice"] in {"dmitri", "irina"}]
    assert len(seeds) == 80, len(seeds)
    cached = {Path(r["path"]).name: read_pcm16(ROOT / "build/re8-probe/audio" / Path(r["path"]).name) for r in seeds}
    OUTPUT.mkdir(parents=True)
    rng = np.random.default_rng(20261025)
    manifest = []
    for index in range(5000):
        seed = seeds[index % len(seeds)]
        audio, parameters = augment(cached[Path(seed["path"]).name], rng)
        name = f"confuser_{index:05d}.wav"
        write_pcm16(OUTPUT / name, audio)
        manifest.append({"path": name, "source_file": Path(seed["path"]).name, "voice": seed["voice"],
                         "phrase": seed["phrase"], "parameters": parameters})
        if (index + 1) % 1000 == 0:
            print(f"Generated {index + 1}/5000", flush=True)
    (OUTPUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")


if __name__ == "__main__":
    main()
