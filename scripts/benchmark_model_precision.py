#!/usr/bin/env python3
"""Run hstream precision/stitching benchmarks and summarize tracked objects."""

from __future__ import annotations

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


REPO_ROOT = Path(__file__).resolve().parents[1]
FP32_CONFIG = REPO_ROOT / "configs" / "config_infer_yolov8_hockey.yaml"
INT8_CONFIG = REPO_ROOT / "configs" / "config_infer_yolov8_hockey_int8.yaml"
BF16_CONFIG = REPO_ROOT / "configs" / "config_infer_yolov8_hockey_bf16.yaml"
PERF_RE = re.compile(r"\*\*PERF:\s+[-0-9.]+\s+\(([-0-9.]+)\)")
INT8_ENGINE = REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx_b2_gpu0_int8.engine"
INT8_CALIB_TABLE = REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_int8_calib.table"
BF16_ENGINE = REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx_b2_gpu0_bf16.engine"


@dataclass(frozen=True)
class Variant:
  name: str
  model_precision: str
  stitch_precision: str


def parse_variants(raw: str) -> list[Variant]:
  presets = {
      "default": [
          Variant("fp32_stitch_fp32", "fp32", "fp32"),
          Variant("fp32_stitch_fp16", "fp32", "fp16"),
          Variant("fp16_stitch_fp32", "fp16", "fp32"),
          Variant("fp16_stitch_fp16", "fp16", "fp16"),
          Variant("int8_stitch_fp32", "int8", "fp32"),
          Variant("int8_stitch_fp16", "int8", "fp16"),
      ],
      "quick": [
          Variant("fp32_stitch_fp32", "fp32", "fp32"),
          Variant("fp32_stitch_fp16", "fp32", "fp16"),
          Variant("bf16_stitch_fp16", "bf16", "fp16"),
          Variant("int8_stitch_fp16", "int8", "fp16"),
      ],
  }
  if raw in presets:
    return presets[raw]
  variants: list[Variant] = []
  for item in raw.split(","):
    item = item.strip()
    if not item:
      continue
    parts = item.split(":")
    if len(parts) != 2:
      raise ValueError(f"variant '{item}' must be model:stitch, e.g. fp32:fp16")
    model, stitch = parts
    variants.append(Variant(f"{model}_stitch_{stitch}", model, stitch))
  return variants


def replace_yaml_scalar(text: str, key: str, value: str) -> str:
  pattern = re.compile(rf"^(\s*{re.escape(key)}:\s*).*$", re.MULTILINE)
  if not pattern.search(text):
    raise ValueError(f"could not find YAML key '{key}'")
  return pattern.sub(rf"\1{value}", text)


def infer_config_for_variant(variant: Variant, out_dir: Path) -> Path | None:
  if variant.model_precision == "fp32":
    return None
  out_path = out_dir / "configs" / f"config_infer_yolov8_hockey_{variant.model_precision}.yaml"
  out_path.parent.mkdir(parents=True, exist_ok=True)

  if variant.model_precision == "fp16":
    text = FP32_CONFIG.read_text()
    text = replace_yaml_scalar(
        text,
        "onnx-file",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx").resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "model-engine-file",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx_b2_gpu0_fp16.engine").resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "labelfile-path",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/labels_coco.txt").resolve()),
    )
    text = replace_yaml_scalar(text, "custom-lib-path", str((REPO_ROOT / "lib/libnvdsinfer_custom_impl_Yolo.so").resolve()))
    text = replace_yaml_scalar(text, "network-mode", "2")
  elif variant.model_precision == "int8":
    text = INT8_CONFIG.read_text()
    text = replace_yaml_scalar(
        text,
        "onnx-file",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx").resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "model-engine-file",
        str(INT8_ENGINE.resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "int8-calib-file",
        str(INT8_CALIB_TABLE.resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "labelfile-path",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/labels_coco.txt").resolve()),
    )
    text = replace_yaml_scalar(text, "custom-lib-path", str((REPO_ROOT / "lib/libnvdsinfer_custom_impl_Yolo.so").resolve()))
  elif variant.model_precision == "bf16":
    text = BF16_CONFIG.read_text()
    text = replace_yaml_scalar(
        text,
        "onnx-file",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/hm_crowdhuman_e85_yolov8_m_1984_736_b2_1984x736.onnx").resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "model-engine-file",
        str(BF16_ENGINE.resolve()),
    )
    text = replace_yaml_scalar(
        text,
        "labelfile-path",
        str((REPO_ROOT / "pretrained/deepstream/yolov8/labels_coco.txt").resolve()),
    )
    text = replace_yaml_scalar(text, "custom-lib-path", str((REPO_ROOT / "lib/libnvdsinfer_custom_impl_Yolo.so").resolve()))
  else:
    raise ValueError(f"unsupported model precision '{variant.model_precision}'")

  out_path.write_text(text)
  return out_path


def has_nonempty_file(path: Path) -> bool:
  return path.exists() and path.stat().st_size > 0


def calibrated_int8_ready() -> tuple[bool, str]:
  missing = []
  if not has_nonempty_file(INT8_CALIB_TABLE):
    missing.append(f"missing/empty calibration table: {INT8_CALIB_TABLE}")
  if not has_nonempty_file(INT8_ENGINE):
    missing.append(f"missing/empty INT8 engine: {INT8_ENGINE}")
  if missing:
    return False, "; ".join(missing)
  return True, ""


def bf16_ready() -> tuple[bool, str]:
  if not has_nonempty_file(BF16_ENGINE):
    return False, f"missing/empty BF16 engine: {BF16_ENGINE}"
  return True, ""


def int8_artifact_requirement(reason: str) -> str:
  return (
      f"calibrated INT8 artifacts required ({reason}); provide a pre-generated non-empty "
      f"calibration table at {INT8_CALIB_TABLE} and INT8 engine at {INT8_ENGINE}, or rerun with --calibrate-int8"
  )


def bf16_artifact_requirement(reason: str) -> str:
  return f"BF16 engine required ({reason}); provide {BF16_ENGINE}, or rerun with --build-bf16"


def build_command(args: argparse.Namespace, variant: Variant, run_dir: Path, infer_config: Path | None) -> list[str]:
  track_dir = run_dir / "tracks"
  track_dir.mkdir(parents=True, exist_ok=True)
  cmd = [
      str(REPO_ROOT / "run.sh"),
      f"--game-id={args.game_id}",
      "--enable-sinks=FAKE",
      f"-t={args.time_limit}",
      f"--stitcher-compute={variant.stitch_precision}",
      f"--options=pipeline.application.kitti-track-dir-path={track_dir}",
  ]
  if args.stitcher_minimize_blend:
    cmd.append("--stitcher-minimize-blend")
  if infer_config is not None:
    cmd.append(f"--options=pipeline.primary-gie.config-file={infer_config}")
  if variant.model_precision == "int8" and args.calibrate_int8 and not args._int8_calibration_completed:
    cmd.append("--models-int8-calibrate")
    cmd.append(f"--int8-calib-frames={args.int8_calib_frames}")
    cmd.append(f"--int8-calib-batch-size={args.int8_calib_batch_size}")
    if args.int8_calib_start_seconds is not None:
      cmd.append(f"--int8-calib-start-seconds={args.int8_calib_start_seconds}")
  elif variant.model_precision == "bf16" and args.build_bf16 and not args._bf16_build_completed:
    cmd.append("--models-bf16-build")
  elif variant.model_precision == "int8" and infer_config is None:
    cmd.append("--models-int8")
  elif variant.model_precision == "bf16" and infer_config is None:
    cmd.append("--models-bf16")
  cmd.extend(args.extra_run_arg)
  return cmd


def parse_perf(log_text: str) -> float | None:
  matches = PERF_RE.findall(log_text)
  if not matches:
    return None
  return float(matches[-1])


def iter_track_files(track_dir: Path) -> Iterable[Path]:
  if not track_dir.exists():
    return []
  return sorted(track_dir.glob("*.txt"))


def summarize_tracks(track_dir: Path) -> dict[str, object]:
  unique_ids: set[str] = set()
  per_frame_counts: list[int] = []
  class_counts: dict[str, int] = {}
  total_observations = 0

  for path in iter_track_files(track_dir):
    count = 0
    for line in path.read_text(errors="replace").splitlines():
      fields = line.split()
      if len(fields) < 2:
        continue
      label, track_id = fields[0], fields[1]
      unique_ids.add(track_id)
      class_counts[label] = class_counts.get(label, 0) + 1
      count += 1
    per_frame_counts.append(count)
    total_observations += count

  avg_active = sum(per_frame_counts) / len(per_frame_counts) if per_frame_counts else 0.0
  max_active = max(per_frame_counts) if per_frame_counts else 0
  return {
      "track_files": len(per_frame_counts),
      "tracked_observations": total_observations,
      "unique_tracked_objects": len(unique_ids),
      "avg_active_tracks_per_frame": avg_active,
      "max_active_tracks_per_frame": max_active,
      "class_counts": dict(sorted(class_counts.items())),
  }


def compare_to_baseline(result: dict[str, object], baseline: dict[str, object]) -> dict[str, float]:
  metrics = {}
  for key in ("track_files", "tracked_observations", "unique_tracked_objects", "avg_active_tracks_per_frame"):
    base = float(baseline.get(key, 0) or 0)
    value = float(result.get(key, 0) or 0)
    if base:
      metrics[f"{key}_delta_pct"] = 100.0 * (value - base) / base
  return metrics


def evaluate_production_gate(args: argparse.Namespace, results: list[dict[str, object]]) -> tuple[bool, str]:
  if args.no_production_gate:
    return True, "production gate disabled"

  baseline = next((r for r in results if r.get("variant") == args.baseline), None)
  candidate = next((r for r in results if r.get("variant") == args.production_candidate), None)
  if baseline is None:
    return False, f"baseline variant '{args.baseline}' was not run"
  if candidate is None:
    return False, f"production candidate '{args.production_candidate}' was not run"
  if baseline.get("status") != "ok":
    return False, f"baseline variant '{args.baseline}' did not complete"
  if candidate.get("status") != "ok":
    return False, f"production candidate '{args.production_candidate}' did not complete: {candidate.get('reason', '')}"

  required_nonzero_metrics = ("track_files", "tracked_observations", "unique_tracked_objects", "avg_active_tracks_per_frame")
  for metric in required_nonzero_metrics:
    if float(baseline.get(metric, 0) or 0) <= 0:
      return False, f"baseline variant '{args.baseline}' produced zero {metric}"
    if float(candidate.get(metric, 0) or 0) <= 0:
      return False, f"production candidate '{args.production_candidate}' produced zero {metric}"

  deltas = compare_to_baseline(candidate, baseline)
  failures = []
  checks = [
      ("unique_tracked_objects_delta_pct", args.max_unique_tracks_delta_pct, "unique tracked objects"),
      ("track_files_delta_pct", args.max_track_files_delta_pct, "track files"),
      ("tracked_observations_delta_pct", args.max_tracked_observations_delta_pct, "tracked observations"),
      ("avg_active_tracks_per_frame_delta_pct", args.max_avg_active_tracks_delta_pct, "avg active tracks/frame"),
  ]
  for key, limit, label in checks:
    value = abs(float(deltas.get(key, 0.0)))
    if value > limit:
      failures.append(f"{label} drift {value:.2f}% > {limit:.2f}%")

  if failures:
    return False, "; ".join(failures)
  return True, (
      f"{args.production_candidate} within tracked-object drift thresholds vs {args.baseline} "
      f"(unique <= {args.max_unique_tracks_delta_pct:.2f}%, "
      f"track files <= {args.max_track_files_delta_pct:.2f}%, "
      f"observations <= {args.max_tracked_observations_delta_pct:.2f}%, "
      f"active/frame <= {args.max_avg_active_tracks_delta_pct:.2f}%)"
  )


def run_variant(args: argparse.Namespace, variant: Variant, out_dir: Path) -> dict[str, object]:
  run_dir = out_dir / variant.name
  if run_dir.exists():
    shutil.rmtree(run_dir)
  run_dir.mkdir(parents=True)

  try:
    infer_config = infer_config_for_variant(variant, out_dir)
  except NotImplementedError as exc:
    return {"variant": variant.name, "status": "skipped", "reason": str(exc)}

  if variant.model_precision == "int8" and not args.calibrate_int8:
    ready, reason = calibrated_int8_ready()
    if not ready:
      return {
          "variant": variant.name,
          "model_precision": variant.model_precision,
          "stitch_precision": variant.stitch_precision,
          "status": "skipped",
          "reason": int8_artifact_requirement(reason),
      }
  if variant.model_precision == "bf16" and not args.build_bf16:
    ready, reason = bf16_ready()
    if not ready:
      return {
          "variant": variant.name,
          "model_precision": variant.model_precision,
          "stitch_precision": variant.stitch_precision,
          "status": "skipped",
          "reason": bf16_artifact_requirement(reason),
      }

  cmd = build_command(args, variant, run_dir, infer_config)
  log_path = run_dir / "run.log"
  started = time.monotonic()
  env = os.environ.copy()
  timed_out = False
  with log_path.open("w") as log:
    try:
      timeout = args.variant_timeout_seconds if args.variant_timeout_seconds > 0 else None
      proc = subprocess.run(
          cmd,
          cwd=REPO_ROOT,
          env=env,
          stdout=log,
          stderr=subprocess.STDOUT,
          text=True,
          timeout=timeout,
      )
    except subprocess.TimeoutExpired:
      timed_out = True
      proc = subprocess.CompletedProcess(cmd, 124)
  wall_seconds = time.monotonic() - started
  log_text = log_path.read_text(errors="replace")

  result: dict[str, object] = {
      "variant": variant.name,
      "model_precision": variant.model_precision,
      "stitch_precision": variant.stitch_precision,
      "status": "ok" if proc.returncode == 0 else "failed",
      "returncode": proc.returncode,
      "wall_seconds": wall_seconds,
      "avg_fps": parse_perf(log_text),
      "log": str(log_path),
      "track_dir": str(run_dir / "tracks"),
  }
  result.update(summarize_tracks(run_dir / "tracks"))
  if proc.returncode != 0:
    if timed_out:
      result["reason"] = f"command timed out after {args.variant_timeout_seconds}s; see {log_path}"
    else:
      result["reason"] = f"command failed; see {log_path}"
  elif variant.model_precision == "int8":
    expected_engine = str(INT8_ENGINE.resolve())
    if expected_engine not in log_text or "Use deserialized engine model" not in log_text:
      result["status"] = "failed"
      result["reason"] = f"INT8 run did not log loading expected engine {expected_engine}; see {log_path}"
  elif variant.model_precision == "bf16":
    expected_engine = str(BF16_ENGINE.resolve())
    if expected_engine not in log_text or "Use deserialized engine model" not in log_text:
      result["status"] = "failed"
      result["reason"] = f"BF16 run did not log loading expected engine {expected_engine}; see {log_path}"
  elif variant.model_precision == "int8" and args.calibrate_int8:
    args._int8_calibration_completed = True
  if result["status"] == "ok" and variant.model_precision == "int8" and args.calibrate_int8:
    args._int8_calibration_completed = True
  if result["status"] == "ok" and variant.model_precision == "bf16" and args.build_bf16:
    args._bf16_build_completed = True
  return result


def print_report(results: list[dict[str, object]], baseline_name: str, gate_ok: bool, gate_reason: str) -> None:
  baseline = next((r for r in results if r.get("variant") == baseline_name and r.get("status") == "ok"), None)
  if baseline:
    for result in results:
      if result.get("status") == "ok":
        result.update(compare_to_baseline(result, baseline))

  print("\nPrecision/tracking benchmark")
  print("variant,status,avg_fps,wall_seconds,track_files,unique_tracked_objects,tracked_observations,avg_active_tracks_per_frame,track_files_delta_pct,unique_delta_pct,obs_delta_pct")
  for r in results:
    print(
        ",".join(
            [
                str(r.get("variant", "")),
                str(r.get("status", "")),
                "" if r.get("avg_fps") is None else f"{float(r['avg_fps']):.2f}",
                "" if r.get("wall_seconds") is None else f"{float(r['wall_seconds']):.3f}",
                str(r.get("track_files", "")),
                str(r.get("unique_tracked_objects", "")),
                str(r.get("tracked_observations", "")),
                "" if r.get("avg_active_tracks_per_frame") is None else f"{float(r['avg_active_tracks_per_frame']):.3f}",
                "" if r.get("track_files_delta_pct") is None else f"{float(r['track_files_delta_pct']):+.2f}",
                "" if r.get("unique_tracked_objects_delta_pct") is None else f"{float(r['unique_tracked_objects_delta_pct']):+.2f}",
                "" if r.get("tracked_observations_delta_pct") is None else f"{float(r['tracked_observations_delta_pct']):+.2f}",
            ]
        )
    )
    if r.get("status") != "ok" and r.get("reason"):
      print(f"  reason: {r['reason']}")
  print(f"\nProduction gate: {'PASS' if gate_ok else 'FAIL'} - {gate_reason}")


def main() -> int:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--game-id", required=True)
  parser.add_argument("-t", "--time-limit", type=int, default=10)
  parser.add_argument("--out-dir", type=Path, default=REPO_ROOT / ".cache" / "precision-benchmark")
  parser.add_argument("--variants", default="quick", help="'quick', 'default', or comma list like fp32:fp32,int8:fp16")
  parser.add_argument("--baseline", default="fp32_stitch_fp32")
  parser.add_argument("--production-candidate", default="int8_stitch_fp16")
  parser.add_argument("--no-production-gate", action="store_true")
  parser.add_argument("--max-unique-tracks-delta-pct", type=float, default=5.0)
  parser.add_argument("--max-track-files-delta-pct", type=float, default=5.0)
  parser.add_argument("--max-tracked-observations-delta-pct", type=float, default=10.0)
  parser.add_argument("--max-avg-active-tracks-delta-pct", type=float, default=10.0)
  parser.add_argument("--stitcher-minimize-blend", action="store_true")
  parser.add_argument(
      "--calibrate-int8",
      action="store_true",
      help="Build missing INT8 calibration artifacts before running INT8 variants.",
  )
  parser.add_argument("--build-bf16", action="store_true", help="Build missing BF16 engine before running BF16 variants.")
  parser.add_argument("--int8-calib-frames", type=int, default=64)
  parser.add_argument("--int8-calib-batch-size", type=int, default=2)
  parser.add_argument("--int8-calib-start-seconds", type=float)
  parser.add_argument(
      "--variant-timeout-seconds",
      type=int,
      default=0,
      help="Optional per-variant timeout. Default 0 means no timeout.",
  )
  parser.add_argument("--extra-run-arg", action="append", default=[])
  args = parser.parse_args()
  args._int8_calibration_completed = False
  args._bf16_build_completed = False

  variants = parse_variants(args.variants)
  args.out_dir.mkdir(parents=True, exist_ok=True)
  results = [run_variant(args, variant, args.out_dir) for variant in variants]
  gate_ok, gate_reason = evaluate_production_gate(args, results)

  report_path = args.out_dir / "summary.json"
  report_path.write_text(
      json.dumps(
          {
              "production_gate": {
                  "ok": gate_ok,
                  "reason": gate_reason,
                  "baseline": args.baseline,
                  "candidate": args.production_candidate,
              },
              "results": results,
          },
          indent=2,
      )
      + "\n"
  )
  print_report(results, args.baseline, gate_ok, gate_reason)
  print(f"\nJSON summary: {report_path}")
  return 1 if any(r.get("status") == "failed" for r in results) or not gate_ok else 0


if __name__ == "__main__":
  raise SystemExit(main())
