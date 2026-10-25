#!/usr/bin/env python3
"""Evaluate the exported streaming microWakeWord model on held-out board audio."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from re8_prepare_mww import OUTPUT, entries, feature
import numpy as np
from microwakeword.inference import Model

MODEL = OUTPUT / "model/tflite_stream_state_internal_quant/stream_state_internal_quant.tflite"


def probabilities(row: dict, model_path: Path) -> list[float]:
    # Separate recordings start with a fresh streaming state.
    model = Model(str(model_path), stride=3)
    spectrogram = np.vstack((np.zeros((99, 40), dtype=np.float32), feature(row, [])))
    values = np.asarray(model.predict_spectrogram(spectrogram), dtype=np.float32)
    values = values[33:]  # 0.99 s of silence warms the streaming state before the clip
    return np.convolve(values, np.ones(3, dtype=np.float32) / 3, mode="valid").tolist()


def events(values: list[float], cutoff: float) -> int:
    accepted = -50
    count = 0
    for index, value in enumerate(values):
        if value > cutoff and index - accepted >= 50:  # 1.5 s cooldown at 30 ms stride
            count += 1
            accepted = index
    return count


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", type=Path, default=MODEL)
    parser.add_argument("--output", type=Path, default=OUTPUT / "heldout_eval.json")
    args = parser.parse_args()
    if not args.model.is_file():
        raise FileNotFoundError(args.model)
    chosen = [r for r in entries() if
              (r["kind"] == "positive_real" and r["split"] in ("validation", "testing")) or
              (r["kind"].startswith("negative_") and r["split"] in ("validation", "testing")) or
              (r["kind"] == "command_eval") or
              (r["kind"] == "ambient" and r["split"] in ("validation_ambient", "testing_ambient"))]
    rows = []
    for index, row in enumerate(chosen, 1):
        values = probabilities(row, args.model)
        rows.append({"kind": row["kind"], "split": row["split"], "origin": row["origin"],
                     "path": row["path"], "score": max(values), "probabilities": values if row["kind"] == "ambient" else None})
        if index % 25 == 0:
            print(f"Evaluated {index}/{len(chosen)}", flush=True)

    validation_negatives = [r["score"] for r in rows if (r["split"] == "validation" and r["kind"].startswith("negative_"))]
    validation_negatives += [max(r["probabilities"]) for r in rows if r["split"] == "validation_ambient"]
    cutoff = max(validation_negatives)

    def summary(split: str) -> dict:
        positives = [r for r in rows if r["split"] == split and r["kind"] == "positive_real"]
        negatives = [r for r in rows if r["split"] == split and r["kind"].startswith("negative_")]
        ambient_split = split + "_ambient"
        ambient = [r for r in rows if r["split"] == ambient_split]
        ambient_seconds = sum(30 for _ in ambient)
        ambient_events = sum(events(r["probabilities"], cutoff) for r in ambient)
        return {"positive_detected": sum(r["score"] > cutoff for r in positives), "positive_total": len(positives),
                "negative_false_accepts": sum(r["score"] > cutoff for r in negatives), "negative_total": len(negatives),
                "ambient_false_accepts": ambient_events, "ambient_seconds": ambient_seconds,
                "ambient_false_accepts_per_hour": ambient_events * 3600 / ambient_seconds}

    commands = [r for r in rows if r["kind"] == "command_eval"]
    report = {"model": str(args.model), "cutoff_selected_on_validation": cutoff,
              "validation": summary("validation"), "testing": summary("testing"),
              "commands_detected": sum(r["score"] > cutoff for r in commands), "commands_total": len(commands),
              "rows": rows,
              "validation_positive_scores": sorted(r["score"] for r in rows if r["split"] == "validation" and r["kind"] == "positive_real"),
              "testing_positive_scores": sorted(r["score"] for r in rows if r["split"] == "testing" and r["kind"] == "positive_real"),
              "testing_command_scores": sorted(r["score"] for r in commands)}
    args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in report.items() if k != "rows" and not k.endswith("_scores")}, ensure_ascii=False, indent=2), flush=True)


if __name__ == "__main__":
    main()
