#!/usr/bin/env python3
"""Create a reproducible synthetic positive corpus for RE-8.

The input clips must be Piper renders of the exact wake word.  The generated
clips stay at 16 kHz, mono, signed PCM so they can be mixed later with real
board recordings.  This script does not train or export a device model.
"""

from __future__ import annotations

import argparse
import json
import math
import random
import wave
from pathlib import Path

import numpy as np

SAMPLE_RATE = 16_000


def read_pcm16(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as source:
        if source.getnchannels() != 1 or source.getsampwidth() != 2:
            raise ValueError(f"Expected 16-bit mono WAV: {path}")
        frames = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2")
        rate = source.getframerate()
    audio = frames.astype(np.float32) / 32768.0
    if rate == SAMPLE_RATE:
        return audio
    positions = np.linspace(0, len(audio) - 1, round(len(audio) * SAMPLE_RATE / rate))
    return np.interp(positions, np.arange(len(audio)), audio).astype(np.float32)


def time_scale(audio: np.ndarray, scale: float) -> np.ndarray:
    length = max(1, round(len(audio) * scale))
    positions = np.linspace(0, len(audio) - 1, length)
    return np.interp(positions, np.arange(len(audio)), audio).astype(np.float32)


def pink_noise(rng: np.random.Generator, length: int) -> np.ndarray:
    white = rng.normal(0.0, 1.0, length + 1).astype(np.float32)
    return np.cumsum(white)[:length] / math.sqrt(length)


def augment(source: np.ndarray, rng: np.random.Generator) -> tuple[np.ndarray, dict[str, float]]:
    speed = float(rng.uniform(0.80, 1.25))
    speech = time_scale(source, speed)
    pre_ms = int(rng.integers(120, 950))
    post_ms = int(rng.integers(280, 1_250))
    output = np.pad(speech, (pre_ms * 16, post_ms * 16))

    # A few small echoes approximate different rooms without claiming to model
    # the actual enclosure.  Board recordings will cover that later.
    echo_delay_ms = int(rng.integers(18, 85))
    echo_gain = float(rng.uniform(0.03, 0.18))
    delay = echo_delay_ms * 16
    if delay < len(output):
        output[delay:] += output[:-delay] * echo_gain

    noise_kind = "white" if rng.random() < 0.5 else "pink"
    noise = rng.normal(0.0, 1.0, len(output)).astype(np.float32)
    if noise_kind == "pink":
        noise = pink_noise(rng, len(output))
    noise /= max(float(np.std(noise)), 1e-5)
    speech_rms = max(float(np.sqrt(np.mean(output * output))), 0.004)
    snr_db = float(rng.uniform(12.0, 32.0))
    output += noise * speech_rms / (10.0 ** (snr_db / 20.0))

    gain_db = float(rng.uniform(-10.0, 3.0))
    output *= 10.0 ** (gain_db / 20.0)
    output = np.clip(output, -0.98, 0.98)
    return output, {
        "speed": round(speed, 4),
        "pre_ms": pre_ms,
        "post_ms": post_ms,
        "echo_delay_ms": echo_delay_ms,
        "echo_gain": round(echo_gain, 4),
        "noise": noise_kind,
        "snr_db": round(snr_db, 2),
        "gain_db": round(gain_db, 2),
    }


def write_pcm16(path: Path, audio: np.ndarray) -> None:
    encoded = (audio * 32767.0).astype("<i2").tobytes()
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(SAMPLE_RATE)
        output.writeframes(encoded)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--count", type=int, default=5_000)
    parser.add_argument("--random-seed", type=int, default=20261023)
    args = parser.parse_args()
    if args.count <= 0:
        raise SystemExit("--count must be positive")

    seeds = sorted(args.seed_dir.glob("*_0_*.wav"))
    if not seeds:
        raise SystemExit(f"No Piper positive seeds in {args.seed_dir}")
    args.output_dir.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(args.random_seed)
    manifest: list[dict[str, object]] = []
    cached = {path: read_pcm16(path) for path in seeds}

    for index in range(args.count):
        source_path = seeds[index % len(seeds)]
        audio, parameters = augment(cached[source_path], rng)
        filename = f"rockserver_{index:05d}.wav"
        write_pcm16(args.output_dir / filename, audio)
        manifest.append({
            "path": filename,
            "label": "positive",
            "transcript": "Роксервер",
            "source": "Piper",
            "source_file": source_path.name,
            "sample_rate_hz": SAMPLE_RATE,
            "parameters": parameters,
        })
        if (index + 1) % 500 == 0 or index + 1 == args.count:
            print(f"Generated {index + 1}/{args.count}", flush=True)

    (args.output_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8"
    )
    (args.output_dir / "README.txt").write_text(
        "Synthetic positive RE-8 corpus. Source: Piper renders of Роксервер.\n"
        "Each WAV is 16 kHz, mono, PCM16. See manifest.json for deterministic\n"
        "augmentation parameters. These clips are for bootstrapping only; do not\n"
        "use them as the sole acceptance or FAR/FRR dataset.\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    main()
