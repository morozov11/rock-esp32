#!/usr/bin/env python3
"""Fine-tune v5 on paired music-only / music-plus-wake-word examples.

Run with C:\\Python311\\python.exe from the repository root. All generated data
and weights stay in build/re8-training/mww-v6c.
"""

from __future__ import annotations

import argparse
import json
import sys
import wave
from pathlib import Path
from types import SimpleNamespace

from re8_prepare_mww import ROOT, RATE, WINDOW, entries, pcm

sys.path[:0] = [str(ROOT / "build/re8-training-env"), str(ROOT / "build/re8-training-source")]
import numpy as np
import tensorflow as tf
from microwakeword.audio.audio_utils import generate_features_for_clip
from microwakeword import mixednet

OUT = ROOT / "build/re8-training/mww-v6c"
WEIGHTS = ROOT / "artifacts/re8-rockserver-mww-v5-candidate.weights.h5"
MUSIC = [
    ROOT / "build/re8-music-v6/false-detections/20261026T143127Z_seq0006_id0001.wav",
    ROOT / "build/re8-board-corpus/sessions/20261023T145710Z_ambient-radio-moderate-01_0043.wav",
    ROOT / "build/re8-music-v6/sessions/20261026T141522Z_negative_music_external_01_0001.wav",
]
RUN2_MUSIC = [
    ROOT / "build/re8-music-v6/false-detections/20261026T145916Z_seq0026_id0002.wav",
    ROOT / "build/re8-music-v6/sessions/20261026T145309Z_negative_music_external_03_0003.wav",
]
LIVE = ROOT / "build/re8-music-v6/sessions/20261026T150538Z_positive_music_live_01_0004.wav"
FRAMES = 204


def save_wav(path: Path, audio: np.ndarray) -> None:
    with wave.open(str(path), "wb") as wav:
        wav.setparams((1, 2, RATE, 0, "NONE", "not compressed"))
        wav.writeframes(audio.astype("<i2").tobytes())


def prepare(count: int, seed: int, include_live: bool) -> tuple[np.ndarray, np.ndarray, list[dict]]:
    rng = np.random.default_rng(seed)
    voices = [Path(row["path"]) for row in entries()
              if row["kind"] == "positive_real" and row["split"] == "training"
              and len(pcm(Path(row["path"]))) <= int(1.8 * RATE)]
    if not voices or not WEIGHTS.is_file():
        raise FileNotFoundError("training voices or v5 weights missing")
    music = [pcm(path) for path in MUSIC]
    positives, negatives, manifest = [], [], []
    samples = OUT / "samples"
    samples.mkdir(parents=True, exist_ok=True)
    for index in range(count):
        source = index % len(music)
        background = music[source]
        if len(background) >= WINDOW:
            start = int(rng.integers(0, len(background) - WINDOW + 1))
            base = background[start:start + WINDOW].astype(np.float32)
        else:
            start = 0
            # The detected false-positive snippet is two seconds; keep its end at the decision point.
            base = np.pad(background.astype(np.float32), (WINDOW - len(background), 0))
        voice_path = voices[int(rng.integers(len(voices)))]
        voice = pcm(voice_path).astype(np.float32)
        post = int(rng.integers(int(.15 * RATE), int(.45 * RATE)))
        offset = WINDOW - post - len(voice)
        if offset < 0:
            raise ValueError(f"voice exceeds paired window: {voice_path}")
        snr = float(rng.choice([0, 5, 10, 15]))
        gain = float(rng.uniform(.7, 1.2))
        music_rms = float(np.sqrt(np.mean(base * base)))
        voice_rms = float(np.sqrt(np.mean(voice * voice)))
        if music_rms < 1 or voice_rms < 1:
            raise ValueError("silent music or voice in training pair")
        scaled_voice = voice * music_rms / voice_rms * 10 ** (snr / 20)
        negative = np.clip(base * gain, -32768, 32767).astype(np.int16)
        mixed = base.copy()
        mixed[offset:offset + len(voice)] += scaled_voice
        positive = np.clip(mixed * gain, -32768, 32767).astype(np.int16)
        assert len(positive) == len(negative) == WINDOW and np.any(positive != negative)
        neg_features = generate_features_for_clip(negative, step_ms=10)[-FRAMES:]
        pos_features = generate_features_for_clip(positive, step_ms=10)[-FRAMES:]
        assert neg_features.shape == pos_features.shape == (FRAMES, 40)
        negatives.append(neg_features.astype(np.float32))
        positives.append(pos_features.astype(np.float32))
        manifest.append({"music": str(MUSIC[source].relative_to(ROOT)), "music_start_sample": start,
                         "voice": str(voice_path.relative_to(ROOT)), "snr_db": snr,
                         "gain": gain, "voice_offset_sample": offset})
        if index < 4:
            save_wav(samples / f"pair_{index:02d}_music.wav", negative)
            save_wav(samples / f"pair_{index:02d}_wake.wav", positive)
        if (index + 1) % 32 == 0:
            print(f"Prepared {index + 1}/{count} pairs", flush=True)
    if include_live:
        live = pcm(LIVE)
        rows = [json.loads(line) for line in (ROOT / "build/re8-music-v6/batch_segments.jsonl").open(encoding="utf-8")]
        rows = [row for row in rows if row["label"] == "positive_music_live_01" and row["cue_index"] <= 3]
        if len(rows) != 3:
            raise ValueError("expected three training utterances in the live music session")
        music_only = live[26 * RATE : 26 * RATE + WINDOW]
        for row in rows:
            start = round((row["cue_seconds"] + .27) * RATE)
            voice_with_music = live[start : start + round(2.25 * RATE)]
            if len(voice_with_music) != round(2.25 * RATE):
                raise ValueError("incomplete live voice window")
            for _ in range(20):
                gain = float(rng.uniform(.7, 1.2))
                pos = np.zeros(WINDOW, dtype=np.float32)
                pos[WINDOW - len(voice_with_music) - RATE // 5 : WINDOW - RATE // 5] = voice_with_music
                pos = np.clip(pos * gain, -32768, 32767).astype(np.int16)
                neg = np.clip(music_only.astype(np.float32) * gain, -32768, 32767).astype(np.int16)
                positives.append(generate_features_for_clip(pos, step_ms=10)[-FRAMES:].astype(np.float32))
                negatives.append(generate_features_for_clip(neg, step_ms=10)[-FRAMES:].astype(np.float32))
                manifest.append({"type": "live_positive_music", "cue_index": row["cue_index"],
                                 "source": str(LIVE.relative_to(ROOT)), "gain": gain,
                                 "negative_window": "26.0-29.2s"})
        for voice_path in voices:
            voice = pcm(voice_path)
            pos = np.zeros(WINDOW, dtype=np.int16)
            pos[WINDOW - len(voice) - RATE // 5 : WINDOW - RATE // 5] = voice
            pos_features = generate_features_for_clip(pos, step_ms=10)[-FRAMES:].astype(np.float32)
            neg_features = generate_features_for_clip(np.zeros(WINDOW, dtype=np.int16), step_ms=10)[-FRAMES:].astype(np.float32)
            for _ in range(2):
                positives.append(pos_features)
                negatives.append(neg_features)
                manifest.append({"type": "clean_replay", "voice": str(voice_path.relative_to(ROOT))})
    (OUT / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return np.stack(positives), np.stack(negatives), manifest


def train(positive: np.ndarray, negative: np.ndarray, steps: int, seed: int,
          positive_weight: float, negative_weight: float, pair_weight: float,
          learning_rate: float, head_only: bool) -> None:
    tf.random.set_seed(seed)
    rng = np.random.default_rng(seed + 1)
    flags = SimpleNamespace(pointwise_filters="64,64,64,64", residual_connection="0,0,0,0",
                            repeat_in_block="1,1,1,1", mixconv_kernel_sizes="[5], [7,11], [9,15], [23]",
                            max_pool=0, first_conv_filters=32, first_conv_kernel_size=5,
                            spatial_attention=0, pooled=0, stride=3)
    model = mixednet.model(flags, (FRAMES, 40), batch_size=16)
    model.load_weights(str(WEIGHTS))
    for layer in model.layers:
        if isinstance(layer, tf.keras.layers.BatchNormalization) or (head_only and
                not isinstance(layer, tf.keras.layers.Dense)):
            layer.trainable = False
    optimizer = tf.keras.optimizers.Adam(learning_rate=learning_rate)
    for step in range(steps):
        choice = rng.integers(0, len(positive), size=8)
        batch = tf.convert_to_tensor(np.concatenate((positive[choice], negative[choice])), dtype=tf.float32)
        with tf.GradientTape() as tape:
            scores = tf.squeeze(model(batch, training=True), axis=-1)
            pos = tf.clip_by_value(scores[:8], 1e-5, 1 - 1e-5)
            neg = tf.clip_by_value(scores[8:], 1e-5, 1 - 1e-5)
            bce = tf.reduce_mean(-positive_weight * tf.math.log(pos) -
                                 negative_weight * tf.math.log1p(-neg))
            pos_logit = tf.math.log(pos) - tf.math.log1p(-pos)
            neg_logit = tf.math.log(neg) - tf.math.log1p(-neg)
            ranking = tf.reduce_mean(tf.nn.softplus(1.0 - pos_logit + neg_logit))
            loss = bce + pair_weight * ranking
        gradients = tape.gradient(loss, model.trainable_variables)
        optimizer.apply_gradients(zip(gradients, model.trainable_variables))
        if (step + 1) % 10 == 0:
            print(f"Step {step + 1}/{steps}: loss={float(loss):.4f} bce={float(bce):.4f} "
                  f"pair={float(ranking):.4f} pos={float(tf.reduce_mean(pos)):.3f} "
                  f"neg={float(tf.reduce_mean(neg)):.3f}", flush=True)
    model_dir = OUT / "model"
    model_dir.mkdir(exist_ok=True)
    model.save_weights(str(model_dir / "best_weights.weights.h5"))
    (model_dir / "last_weights.weights.h5").write_bytes((model_dir / "best_weights.weights.h5").read_bytes())


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pairs", type=int, default=192)
    parser.add_argument("--steps", type=int, default=80)
    parser.add_argument("--seed", type=int, default=8)
    parser.add_argument("--output-name", default="mww-v6c")
    parser.add_argument("--include-run2", action="store_true")
    parser.add_argument("--include-live", action="store_true")
    parser.add_argument("--head-only", action="store_true")
    parser.add_argument("--positive-weight", type=float, default=1.0)
    parser.add_argument("--negative-weight", type=float, default=3.0)
    parser.add_argument("--pair-weight", type=float, default=.5)
    parser.add_argument("--learning-rate", type=float, default=1e-5)
    args = parser.parse_args()
    if args.pairs < 8 or args.steps < 1:
        parser.error("--pairs must be >= 8 and --steps >= 1")
    if not all(x > 0 for x in (args.positive_weight, args.negative_weight,
                              args.pair_weight, args.learning_rate)):
        parser.error("loss weights and learning rate must be positive")
    if not args.output_name.startswith("mww-v6") or not args.output_name.replace("-", "").isalnum():
        parser.error("--output-name must be an mww-v6* directory name")
    global OUT, MUSIC
    OUT = ROOT / "build/re8-training" / args.output_name
    if args.include_run2:
        MUSIC = MUSIC + RUN2_MUSIC
    if (OUT / "model/best_weights.weights.h5").exists():
        raise FileExistsError(f"refusing to overwrite trained weights in {OUT}")
    OUT.mkdir(parents=True, exist_ok=True)
    positive, negative, manifest = prepare(args.pairs, args.seed, args.include_live)
    (OUT / "run.json").write_text(json.dumps(vars(args), indent=2) + "\n", encoding="utf-8")
    print(f"Ready: {len(manifest)} paired examples; training", flush=True)
    train(positive, negative, args.steps, args.seed, args.positive_weight,
          args.negative_weight, args.pair_weight, args.learning_rate, args.head_only)


if __name__ == "__main__":
    main()
