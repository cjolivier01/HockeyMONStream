#!/usr/bin/env python3
"""Compare raw detector output across model precisions.

Runs the pipeline once per precision variant with the primary GIE dumping raw
KITTI detections, then measures how far each candidate drifts from an FP32
reference on the frames both runs produced.

Detector output is compared before the tracker, so this measures the detector
itself rather than the tracker's smoothing of it.

Run-to-run drift is not zero even at identical precision (frame scheduling and
near-threshold boxes both vary), so a `fp32` candidate is included by default as
a control. A candidate is only meaningful if its drift exceeds that control.
Note the control shares stitching state with the other runs, so it bounds
detector plus stitcher variation, not the detector alone.

WARNING: this drives the real pipeline against a real game directory. The
default one-pass flow configures stitching in-process when control masks are
missing, which REWRITES that game's calibration artifacts. Point it at a game
whose stitching is already configured, or at a copy.

  scripts/compare_detection_accuracy.py --game-id=tv-14-1-p1 -t=30
  scripts/compare_detection_accuracy.py --game-id=tv-14-1-p1 --variants=fp16,int8
  scripts/compare_detection_accuracy.py --skip-run --out-dir=/tmp/accuracy-run
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
FP16_CONFIG = REPO_ROOT / "configs" / "config_infer_yolov8_hockey_fp16.yaml"
INT8_CONFIG = REPO_ROOT / "configs" / "config_infer_yolov8_hockey_int8.yaml"
BF16_CONFIG = REPO_ROOT / "configs" / "config_infer_yolov8_hockey_bf16.yaml"
KITTI_NAME_RE = re.compile(r"^(\d+)_(\d+)_(\d+)\.txt$")
# One label token plus 15 numeric fields; the label itself may be multi-word.
NUM_KITTI_NUMERIC = 15
NUM_KITTI_FIELDS = NUM_KITTI_NUMERIC + 1
SUPPORTED_PRECISIONS = {"fp32", "fp16", "int8", "bf16"}


@dataclass(frozen=True)
class Detection:
  label: str
  left: float
  top: float
  right: float
  bottom: float
  confidence: float

  @property
  def area(self) -> float:
    return max(0.0, self.right - self.left) * max(0.0, self.bottom - self.top)


def iou(a: Detection, b: Detection) -> float:
  ix0, iy0 = max(a.left, b.left), max(a.top, b.top)
  ix1, iy1 = min(a.right, b.right), min(a.bottom, b.bottom)
  inter = max(0.0, ix1 - ix0) * max(0.0, iy1 - iy0)
  if inter <= 0.0:
    return 0.0
  union = a.area + b.area - inter
  return inter / union if union > 0.0 else 0.0


def infer_config_for(precision: str) -> Path | None:
  """Return the nvinfer config for `precision`, or None to use the default."""
  if precision == "fp32":
    return None
  if precision == "fp16":
    return FP16_CONFIG
  if precision == "int8":
    return INT8_CONFIG
  if precision == "bf16":
    return BF16_CONFIG
  raise ValueError(f"unsupported precision '{precision}'")


def run_variant(args: argparse.Namespace, variant: str, precision: str, out_dir: Path) -> dict:
  bbox_dir = out_dir / variant / "kitti"
  if bbox_dir.exists():
    shutil.rmtree(bbox_dir)
  bbox_dir.mkdir(parents=True)

  cmd = [
      str(REPO_ROOT / "run.sh"),
      f"--game-id={args.game_id}",
      "--enable-sinks=FAKE",
      f"-t={args.time_limit}",
      f"--options=pipeline.application.bbox-dir-path={bbox_dir}",
  ]
  infer_config = infer_config_for(precision)
  if infer_config is not None:
    cmd.append(f"--options=pipeline.primary-gie.config-file={infer_config}")
  cmd.extend(args.extra_run_arg)

  env = dict(os.environ)
  if args.disable_scoreboard:
    # Not merely "skip the picker": ScoreboardSelector::Run() short-circuits to
    # Save(game_dir, {}), which PERSISTS an empty rink.scoreboard.
    # perspective_polygon into the game's config.yaml. Behind a flag for that
    # reason. Forced, not setdefault, because only exactly "1" disables it.
    env["HM_NO_SCOREBOARD"] = "1"

  log_path = out_dir / variant / "run.log"
  print(f"[{variant}] {' '.join(cmd)}", flush=True)
  try:
    with log_path.open("w") as log:
      proc = subprocess.run(
          cmd, stdout=log, stderr=subprocess.STDOUT, env=env, cwd=REPO_ROOT, timeout=args.run_timeout
      )
    returncode = proc.returncode
  except subprocess.TimeoutExpired:
    returncode = -1
    print(f"[{variant}] timed out after {args.run_timeout}s", file=sys.stderr)
  return {"variant": variant, "precision": precision, "returncode": returncode, "log": str(log_path)}


def load_detections(bbox_dir: Path) -> dict[tuple[int, int], list[Detection]]:
  """Parse the raw primary-GIE KITTI dump written by write_kitti_output().

  Line layout is a label followed by exactly 15 numeric fields:
    label  0.0 0 0.0  left top right bottom  0.0 x7  confidence

  Fields are indexed from the END, because the label is a class name from the
  labels file and COCO's has 13 multi-word entries ("traffic light", "sports
  ball", ...). A two-token label yields 17 fields, which would still satisfy a
  leading-index guard while silently shifting every column.
  """
  frames: dict[tuple[int, int], list[Detection]] = {}
  for path in sorted(bbox_dir.glob("*.txt")):
    match = KITTI_NAME_RE.match(path.name)
    if not match:
      continue
    _, stream_id, frame_num = (int(g) for g in match.groups())
    dets: list[Detection] = []
    for line in path.read_text(errors="replace").splitlines():
      fields = line.split()
      if len(fields) < NUM_KITTI_FIELDS:
        continue
      label = " ".join(fields[:-NUM_KITTI_NUMERIC])
      if not label:
        continue
      try:
        left, top, right, bottom = (float(v) for v in fields[-12:-8])
        dets.append(Detection(label, left, top, right, bottom, float(fields[-1])))
      except ValueError:
        continue
    frames[(stream_id, frame_num)] = dets
  return frames


def match_frame(ref: list[Detection], cand: list[Detection], iou_threshold: float):
  """Greedily pair same-class boxes by descending IoU. Returns (pairs, missed, extra).

  Pairs must agree on class: without that, a label flip on an otherwise
  identical box scores as a perfect IoU-1.0 match. Every box is `person` with
  today's checkpoint, but the config declares num-detected-classes: 80.
  """
  pairs: list[tuple[Detection, Detection, float]] = []
  candidates = sorted(
      (
          (iou(r, c), ri, ci)
          for ri, r in enumerate(ref)
          for ci, c in enumerate(cand)
          if r.label == c.label
      ),
      key=lambda t: t[0],
      reverse=True,
  )
  used_ref: set[int] = set()
  used_cand: set[int] = set()
  for score, ri, ci in candidates:
    if score < iou_threshold:
      break
    if ri in used_ref or ci in used_cand:
      continue
    used_ref.add(ri)
    used_cand.add(ci)
    pairs.append((ref[ri], cand[ci], score))
  missed = [d for i, d in enumerate(ref) if i not in used_ref]
  extra = [d for i, d in enumerate(cand) if i not in used_cand]
  return pairs, missed, extra


def compare(ref_frames, cand_frames, iou_threshold: float, min_confidence: float) -> dict:
  def keep(dets):
    return [d for d in dets if d.confidence >= min_confidence]

  common = sorted(set(ref_frames) & set(cand_frames))
  n_ref = n_cand = n_matched = n_missed = n_extra = n_frames_count_delta = 0
  iou_sum = conf_abs_sum = conf_signed_sum = 0.0
  missed_conf: list[float] = []
  extra_conf: list[float] = []

  for key in common:
    ref = keep(ref_frames[key])
    cand = keep(cand_frames[key])
    n_ref += len(ref)
    n_cand += len(cand)
    if len(ref) != len(cand):
      # The count-delta frame share is what the tracker actually sees.
      n_frames_count_delta += 1
    pairs, missed, extra = match_frame(ref, cand, iou_threshold)
    n_matched += len(pairs)
    n_missed += len(missed)
    n_extra += len(extra)
    missed_conf.extend(d.confidence for d in missed)
    extra_conf.extend(d.confidence for d in extra)
    for r, c, score in pairs:
      iou_sum += score
      conf_abs_sum += abs(r.confidence - c.confidence)
      conf_signed_sum += c.confidence - r.confidence

  return {
      "min_confidence": min_confidence,
      "frames_compared": len(common),
      "frames_ref_only": len(set(ref_frames) - set(cand_frames)),
      "frames_cand_only": len(set(cand_frames) - set(ref_frames)),
      "ref_detections": n_ref,
      "cand_detections": n_cand,
      # None rather than 0.0 when the denominator is empty: a 0.00 here is
      # indistinguishable from "the candidate detected nothing", which is a very
      # different result. Raising --min-confidence above every score hits this.
      "detection_count_delta_pct": (100.0 * (n_cand - n_ref) / n_ref) if n_ref else None,
      "matched": n_matched,
      "missed": n_missed,
      "extra": n_extra,
      # Recall alone rewards a candidate that simply emits more boxes, so
      # precision and F1 are reported too. F1 is the threshold-robust summary.
      "recall_vs_ref_pct": (100.0 * n_matched / n_ref) if n_ref else None,
      "precision_vs_ref_pct": (100.0 * n_matched / n_cand) if n_cand else None,
      "f1_vs_ref_pct": (100.0 * 2 * n_matched / (n_ref + n_cand)) if (n_ref + n_cand) else None,
      # Misses and extras are errors at different locations: they add, not cancel.
      "total_disagreements": n_missed + n_extra,
      "missed_pct": (100.0 * n_missed / n_ref) if n_ref else None,
      "extra_pct": (100.0 * n_extra / n_ref) if n_ref else None,
      "frames_with_count_delta": n_frames_count_delta,
      "mean_iou_matched": (iou_sum / n_matched) if n_matched else None,
      "mean_abs_confidence_delta": (conf_abs_sum / n_matched) if n_matched else None,
      "mean_signed_confidence_delta": (conf_signed_sum / n_matched) if n_matched else None,
      "mean_missed_confidence": (sum(missed_conf) / len(missed_conf)) if missed_conf else None,
      "mean_extra_confidence": (sum(extra_conf) / len(extra_conf)) if extra_conf else None,
  }


def _num(value: float | None, width: int, precision: int, sign: str = "") -> str:
  """Render a metric, or a right-aligned n/a when it was undefined."""
  if value is None:
    return f"{'n/a':>{width}}"
  return f"{value:>{sign}{width}.{precision}f}"


def format_row(variant: str, m: dict) -> str:
  return (
      f"{variant:<16} {m['frames_compared']:>7} {m['ref_detections']:>8} {m['cand_detections']:>8} "
      f"{_num(m['recall_vs_ref_pct'], 8, 2)} {_num(m['precision_vs_ref_pct'], 9, 2)} "
      f"{_num(m['f1_vs_ref_pct'], 7, 2)} {m['missed']:>7} {m['extra']:>6} "
      f"{m['frames_with_count_delta']:>8} {_num(m['mean_iou_matched'], 8, 4)} "
      f"{_num(m['mean_abs_confidence_delta'], 9, 4)}"
  )


HEADER = (
    f"{'variant':<16} {'frames':>7} {'ref det':>8} {'cand det':>8} {'recall%':>8} "
    f"{'precis%':>9} {'F1%':>7} {'missed':>7} {'extra':>6} {'dframes':>8} "
    f"{'meanIoU':>8} {'|dconf|':>9}"
)


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
  parser.add_argument("--game-id", default="tv-14-1-p1")
  parser.add_argument("-t", "--time-limit", type=int, default=30)
  parser.add_argument(
      "--variants",
      default="fp32,fp16",
      help="comma-separated precisions to compare against the fp32 reference. "
      "A bare 'fp32' entry acts as the run-to-run control.",
  )
  parser.add_argument("--out-dir", type=Path, default=Path("/tmp/hstream-accuracy"))
  parser.add_argument("--iou-threshold", type=float, default=0.5)
  parser.add_argument(
      "--min-confidence",
      default="0.25,0.5",
      help="comma-separated confidence floors to report; near-threshold boxes flicker between runs",
  )
  parser.add_argument("--skip-run", action="store_true", help="reuse dumps already in --out-dir")
  parser.add_argument(
      "--run-timeout", type=float, default=1800.0, help="per-variant wall-clock limit in seconds"
  )
  parser.add_argument(
      "--disable-scoreboard",
      action="store_true",
      help="set HM_NO_SCOREBOARD=1 so runs are unattended. This WRITES an empty "
      "rink.scoreboard.perspective_polygon into the game's config.yaml and does not undo it. "
      "Without it, a game lacking scoreboard corners will block on an interactive picker.",
  )
  parser.add_argument("--extra-run-arg", action="append", default=[])
  args = parser.parse_args()

  # Resolve before use: the child runs with cwd=REPO_ROOT, so a relative path
  # would have the parent create one directory and the pipeline write to another.
  out_dir = args.out_dir.resolve()
  out_dir.mkdir(parents=True, exist_ok=True)
  precisions = [p.strip() for p in args.variants.split(",") if p.strip()]
  floors = [float(c) for c in args.min_confidence.split(",") if c.strip()]

  # Validate up front rather than failing partway through a multi-run sweep.
  unsupported = [p for p in precisions if p not in SUPPORTED_PRECISIONS]
  if unsupported:
    print(
        f"unsupported precision(s): {', '.join(unsupported)} "
        f"(expected one of {', '.join(sorted(SUPPORTED_PRECISIONS))})",
        file=sys.stderr,
    )
    return 2

  # The reference is always a dedicated fp32 run; listed variants are candidates.
  # Names are de-duplicated so a repeated precision means "run it again and
  # compare" -- `--variants=fp16,fp16` is the FP16-vs-rebuilt-FP16 control,
  # which matters because TensorRT tactic selection is timing-dependent and
  # FP16 builds are not bit-reproducible.
  plan = [("reference", "fp32")]
  seen: dict[str, int] = {}
  for precision in precisions:
    name = "control-fp32" if precision == "fp32" else precision
    seen[name] = seen.get(name, 0) + 1
    if seen[name] > 1:
      name = f"{name}-{seen[name]}"
    plan.append((name, precision))

  failed: list[str] = []
  if not args.skip_run:
    for variant, precision in plan:
      info = run_variant(args, variant, precision, out_dir)
      if info["returncode"] != 0:
        # Keep going: a missing INT8/BF16 artifact should not throw away the
        # reference and candidate runs that already completed.
        print(f"[{variant}] FAILED rc={info['returncode']}; see {info['log']}", file=sys.stderr)
        failed.append(variant)
        if variant == "reference":
          print("reference run failed; nothing to compare against", file=sys.stderr)
          return 1

  ref_frames = load_detections(out_dir / "reference" / "kitti")
  if not ref_frames:
    print(f"no reference detections under {out_dir / 'reference' / 'kitti'}", file=sys.stderr)
    return 1

  # Parse each candidate once, not once per confidence floor.
  candidates = {v: load_detections(out_dir / v / "kitti") for v, _ in plan[1:]}

  report: dict[str, dict] = {}
  for floor in floors:
    print(f"\n=== detection agreement vs fp32 reference (IoU>={args.iou_threshold}, conf>={floor}) ===")
    print(HEADER)
    for variant, _ in plan[1:]:
      cand_frames = candidates[variant]
      if not cand_frames:
        print(f"{variant:<16} (no detections)")
        continue
      metrics = compare(ref_frames, cand_frames, args.iou_threshold, floor)
      report.setdefault(variant, {})[f"conf_{floor}"] = metrics
      print(format_row(variant, metrics))
      # The headline is an intersection; say so when the runs differ in length.
      if metrics["frames_ref_only"] or metrics["frames_cand_only"]:
        print(
            f"{'':<16} (compared the intersection; {metrics['frames_ref_only']} ref-only "
            f"and {metrics['frames_cand_only']} candidate-only frames excluded)"
        )

  report_path = out_dir / "accuracy_report.json"
  report_path.write_text(json.dumps(report, indent=2))
  print(f"\nwrote {report_path}")

  # Interpretation: a candidate is only suspect if it drifts more than the control.
  if "control-fp32" in report:
    print(
        "\nRead 'control-fp32' as the run-to-run noise floor. A candidate is "
        "indistinguishable from FP32 if its drift is within that row."
    )
  if failed:
    print(f"\nincomplete: {', '.join(failed)} did not run; report covers the rest", file=sys.stderr)
    return 1
  return 0


if __name__ == "__main__":
  sys.exit(main())
