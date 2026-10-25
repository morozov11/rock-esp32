#!/usr/bin/env python3
"""Score a verified board WAV with the frozen RE-8 streaming candidate.

Use the same 10 ms microfrontend, 30 ms model stride, three-score smoothing,
and 0.617 threshold as the held-out PC evaluation. This is a PC-side board
experiment; it does not claim the model already runs on the ESP32-P4.
"""

from __future__ import annotations

import argparse
import json
import sys
import wave
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "build/re8-training-env"), str(ROOT / "build/re8-training-source")]

import numpy as np
from microwakeword.audio.audio_utils import generate_features_for_clip
from microwakeword.inference import Model

MODEL = ROOT / "artifacts/re8-rockserver-mww-v5-candidate-int8.tflite"
THRESHOLD = 0.617


def score(wav_path: Path) -> dict:
    with wave.open(str(wav_path), "rb") as source:
        if (source.getnchannels(), source.getsampwidth(), source.getframerate()) != (1, 2, 16000):
            raise ValueError("expected mono 16-bit PCM at 16 kHz")
        pcm = np.frombuffer(source.readframes(source.getnframes()), dtype="<i2").copy()
    if len(pcm) < 16000:
        raise ValueError("recording must contain at least one second")
    model = Model(str(MODEL), stride=3)
    features = generate_features_for_clip(pcm, step_ms=10)
    # Mirror the held-out evaluation: one second of silence initializes the
    # recurrent state, and consecutive predictions are averaged in threes.
    features = np.vstack((np.zeros((99, 40), dtype=np.float32), features))
    values = np.asarray(model.predict_spectrogram(features), dtype=np.float32)[33:]
    smoothed = np.convolve(values, np.ones(3, dtype=np.float32) / 3, mode="valid")
    detections = []
    accepted = -50
    for index, value in enumerate(smoothed):
        if value > THRESHOLD and index - accepted >= 50:
            detections.append({"seconds": round(index * 0.03, 2), "score": round(float(value), 4)})
            accepted = index
    return {
        "path": str(wav_path),
        "duration_seconds": round(len(pcm) / 16000, 3),
        "threshold": THRESHOLD,
        "max_score": round(float(np.max(smoothed)), 4),
        "detections": detections,
    }


def extract_detection_clips(wav_path: Path, result: dict, output_dir: Path) -> list[str]:
    """Save short, reviewable WAV clips around every accepted detection."""
    with wave.open(str(wav_path), "rb") as source:
        params = source.getparams()
        pcm = source.readframes(source.getnframes())
    output_dir.mkdir(parents=True, exist_ok=True)
    paths = []
    for index, detection in enumerate(result["detections"], 1):
        start_frame = max(0, int((detection["seconds"] - 1.0) * 16000))
        end_frame = min(len(pcm) // 2, start_frame + int(2.5 * 16000))
        path = output_dir / f"{wav_path.stem}_detection{index:02d}_{detection['seconds']:.2f}s.wav"
        with wave.open(str(path), "wb") as clip:
            clip.setparams(params)
            clip.writeframes(pcm[start_frame * 2 : end_frame * 2])
        paths.append(str(path))
    return paths


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--extract-detections", type=Path,
                        help="directory for 2.5-second WAV clips around detected events")
    args = parser.parse_args()
    result = score(args.wav)
    if args.extract_detections:
        result["detection_clips"] = extract_detection_clips(args.wav, result, args.extract_detections)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
