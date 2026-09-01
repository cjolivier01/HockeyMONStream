#!/usr/bin/env python3
"""Capture one stitched calibration panorama for each YAML-defined projection case."""

from __future__ import annotations

import argparse
import csv
import fcntl
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
from types import SimpleNamespace
from typing import Any

import yaml

try:
  import run_stitching_calibration_matrix as calibration_matrix
except ModuleNotFoundError:
  from scripts import run_stitching_calibration_matrix as calibration_matrix


PARAMETER_SPECS: dict[str, tuple[tuple[float, float], ...]] = {
    "albers-equal-area-conic": ((-90, 90), (-90, 90)),
    "biplane": ((1, 179), (0, 1)),
    "triplane": ((1, 120),),
    "general-panini": ((0, 150), (-100, 100), (-100, 100)),
}
SUPPORTED_PROJECTIONS = frozenset(calibration_matrix.PARAMETERLESS_PROJECTIONS) | frozenset(PARAMETER_SPECS)
MANIFEST_FIELDS = (
    "sequence",
    "label",
    "projection",
    "parameters",
    "auto_fov",
    "horizontal_fov",
    "auto_canvas",
    "auto_crop",
    "png",
    "effective_config",
    "log",
    "outcome",
    "duration_seconds",
    "return_code",
    "first_failure",
    "work_directory",
)


def as_map(value: object, path: str) -> dict[str, Any]:
  if not isinstance(value, dict):
    raise ValueError(f"{path} must be a map")
  return value


def as_positive_number(value: object, path: str) -> float:
  if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
    raise ValueError(f"{path} must be a positive finite number")
  return float(value)


def as_integer(value: object, path: str, minimum: int = 1) -> int:
  if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
    raise ValueError(f"{path} must be an integer of at least {minimum}")
  return value


def validate_parameters(projection: str, parameters: object, path: str) -> tuple[float, ...]:
  expected = PARAMETER_SPECS.get(projection, ())
  if parameters is None:
    parameters = []
  if not isinstance(parameters, list) or len(parameters) != len(expected):
    raise ValueError(f"{path} must contain exactly {len(expected)} value(s) for {projection}")
  parsed: list[float] = []
  for index, (value, limits) in enumerate(zip(parameters, expected)):
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value):
      raise ValueError(f"{path}[{index}] must be a finite number")
    numeric = float(value)
    if numeric < limits[0] or numeric > limits[1] or abs(numeric * 100 - round(numeric * 100)) > 1e-7:
      raise ValueError(f"{path}[{index}] must be within {limits} in increments of 0.01")
    parsed.append(numeric)
  if projection == "biplane" and parsed[1] not in (0, 1):
    raise ValueError(f"{path}[1] must be exactly 0 or 1")
  return tuple(parsed)


def maximum_horizontal_fov(projection: str, parameters: tuple[float, ...]) -> float:
  if projection in ("rectilinear", "transverse-mercator"):
    return 179
  if projection in ("stereographic", "panini", "equirectangular-panini"):
    return 359
  if projection == "orthographic":
    return 180
  if projection == "biplane":
    return min(360, parameters[0] + 179)
  if projection == "triplane":
    return min(360, 2 * parameters[0] + 179)
  if projection == "general-panini":
    maximum_angle = math.radians(80)
    compression_scale = (150 - parameters[0]) / 50
    compression = 1.5 / (compression_scale + 0.0001) - 1.5 / 3.0001
    theoretical = math.acos(-1 / compression if compression > 1 else -compression)
    projection_argument = compression * math.sin(maximum_angle)
    half_fov = theoretical
    if projection_argument <= 1:
      half_fov = min(half_fov, math.asin(max(-1, projection_argument)) + maximum_angle)
    return math.degrees(2 * half_fov)
  return 360


def expand_cases(config: dict[str, Any]) -> list[dict[str, object]]:
  if config.get("version") != 1:
    raise ValueError("version must be 1")
  defaults = as_map(config.get("defaults", {}), "defaults")
  projections = config.get("projections")
  if not isinstance(projections, list) or not projections:
    raise ValueError("projections must be a non-empty sequence")
  cases: list[dict[str, object]] = []
  labels: set[str] = set()
  for projection_index, entry_value in enumerate(projections):
    entry = as_map(entry_value, f"projections[{projection_index}]")
    projection = entry.get("name")
    if not isinstance(projection, str) or projection not in SUPPORTED_PROJECTIONS:
      raise ValueError(f"projections[{projection_index}].name is not a supported canonical projection")
    variants = entry.get("variants")
    if not isinstance(variants, list) or not variants:
      raise ValueError(f"projections[{projection_index}].variants must be a non-empty sequence")
    for variant_index, variant_value in enumerate(variants):
      variant = as_map(variant_value, f"projections[{projection_index}].variants[{variant_index}]")
      merged = {**defaults, **variant}
      parameters = validate_parameters(
          projection,
          merged.get("parameters", []),
          f"projections[{projection_index}].variants[{variant_index}].parameters",
      )
      label_value = merged.get("label", f"case-{variant_index + 1}")
      if not isinstance(label_value, str) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", label_value):
        raise ValueError("case labels must contain only letters, digits, dot, underscore, and dash")
      label = f"{projection}--{label_value}"
      if label in labels:
        raise ValueError(f"duplicate case label: {label}")
      labels.add(label)
      auto_fov = merged.get("auto_fov", False)
      auto_canvas = merged.get("auto_canvas", True)
      auto_crop = merged.get("auto_crop", False)
      if not all(isinstance(value, bool) for value in (auto_fov, auto_canvas, auto_crop)):
        raise ValueError(f"{label} framing auto values must be booleans")
      horizontal_fov = as_positive_number(merged.get("horizontal_fov", 180), f"{label}.horizontal_fov")
      if horizontal_fov > 360:
        raise ValueError(f"{label}.horizontal_fov must be at most 360")
      maximum_fov = maximum_horizontal_fov(projection, parameters)
      if not auto_fov and horizontal_fov > maximum_fov + 1e-9:
        raise ValueError(
            f"{label}.horizontal_fov is {horizontal_fov:g}, but {projection} with these parameters allows at most "
            f"{maximum_fov:g} degrees"
        )
      backend = merged.get("mapping_backend", "nona")
      if backend not in ("nona", "opencv-magsac", "opencv-affine-ransac"):
        raise ValueError(f"{label}.mapping_backend is unsupported")
      if backend != "nona" and projection != "rectilinear":
        raise ValueError(f"{label}: {backend} supports only rectilinear projection output")
      cases.append(
          {
              "case": label,
              "label": label_value,
              "backend": backend,
              "projection": projection,
              "parameters": parameters,
              "auto_fov": auto_fov,
              "horizontal_fov": horizontal_fov,
              "auto_canvas": auto_canvas,
              "auto_crop": auto_crop,
              "control_points": as_integer(merged.get("control_points", 900), f"{label}.control_points"),
              "frame_count": as_integer(merged.get("frame_count", 4), f"{label}.frame_count"),
              "stitch_frame_time": str(merged.get("stitch_frame_time", "00:00:00")),
              "max_output_width": as_integer(
                  merged.get("max_output_width", 4096), f"{label}.max_output_width", minimum=0
              ),
          }
      )
  return cases


def number_text(value: float) -> str:
  return f"{value:g}".replace("-", "m").replace(".", "p")


def output_stem(sequence: int, state: dict[str, object]) -> str:
  parameters = tuple(float(value) for value in state["parameters"])
  parameter_part = "none" if not parameters else "-".join(number_text(value) for value in parameters)
  fov_part = "auto" if state["auto_fov"] else number_text(float(state["horizontal_fov"]))
  canvas_part = "auto" if state["auto_canvas"] else "retained"
  crop_part = "auto" if state["auto_crop"] else "full"
  return (
      f"{sequence:03d}__{state['projection']}__params-{parameter_part}__fov-{fov_part}"
      f"__canvas-{canvas_part}__crop-{crop_part}__{state['label']}"
  )


def resolve_path(value: object, base: Path, path: str, required: bool = True) -> Path | None:
  if value is None and not required:
    return None
  if not isinstance(value, str) or not value:
    raise ValueError(f"{path} must be a non-empty path")
  candidate = Path(value).expanduser()
  return (candidate if candidate.is_absolute() else base / candidate).resolve()


def load_config(path: Path) -> dict[str, Any]:
  with path.open("r", encoding="utf-8") as stream:
    config = yaml.safe_load(stream) or {}
  return as_map(config, "configuration root")


def effective_case_config(state: dict[str, object]) -> dict[str, object]:
  return {
      "projection": state["projection"],
      "projection_parameters": list(state["parameters"]),
      "projection_framing": calibration_matrix.projection_framing(state),
      "mapping_backend": state["backend"],
      "run_autooptimizer": state["backend"] == "nona",
      "control_points": state["control_points"],
      "calibration_frame_count": state["frame_count"],
      "stitch_frame_time": state["stitch_frame_time"],
      "max_output_width": state["max_output_width"],
  }


def convert_panorama(source: Path, destination: Path, ffmpeg: str) -> None:
  subprocess.run(
      [ffmpeg, "-v", "error", "-y", "-i", str(source), "-frames:v", "1", "-pix_fmt", "rgb24", str(destination)],
      check=True,
      timeout=120,
  )
  if not destination.is_file() or destination.stat().st_size == 0:
    raise RuntimeError(f"ffmpeg did not create {destination}")


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--config", type=Path, required=True, help="Projection matrix YAML")
  parser.add_argument("--source-game-dir", type=Path, help="Override source_game_dir from YAML")
  parser.add_argument("--output-dir", type=Path, help="Override output_dir from YAML")
  parser.add_argument("--start-at", type=int, default=1, help="One-based first case")
  parser.add_argument("--limit", type=int, help="Maximum number of cases")
  parser.add_argument("--dry-run", action="store_true", help="Validate and list cases without launching pipelines")
  parser.add_argument("--keep-failed-work", action="store_true")
  args = parser.parse_args()
  if args.start_at <= 0 or (args.limit is not None and args.limit <= 0):
    parser.error("--start-at and --limit must be positive")
  args.config = args.config.resolve()
  return args


def main() -> int:
  cli = parse_args()
  config = load_config(cli.config)
  cases = expand_cases(config)
  selected = list(enumerate(cases, start=1))[cli.start_at - 1 :]
  if cli.limit is not None:
    selected = selected[: cli.limit]
  if not selected:
    raise SystemExit("no cases selected")
  for sequence, state in selected:
    print(f"{sequence:03d} {output_stem(sequence, state)}")
  if cli.dry_run:
    print(f"Dry run: {len(selected)} of {len(cases)} cases")
    return 0

  base = cli.config.parent
  pipeline = as_map(config.get("pipeline", {}), "pipeline")
  resources = as_map(config.get("resources", {}), "resources")
  source_game_dir = (cli.source_game_dir.resolve() if cli.source_game_dir else resolve_path(config.get("source_game_dir"), base, "source_game_dir"))
  output_dir = (cli.output_dir.resolve() if cli.output_dir else resolve_path(config.get("output_dir"), base, "output_dir"))
  workspace = resolve_path(pipeline.get("workspace", ".."), base, "pipeline.workspace")
  executable = resolve_path(pipeline.get("executable", "bazel-bin/src/apps/pipeline-app/pipeline-app"), workspace, "pipeline.executable")
  config_root = resolve_path(pipeline.get("config_root", "configs"), workspace, "pipeline.config_root")
  pipeline_config = resolve_path(pipeline.get("config", "configs/ds_hockey_app_config.yaml"), workspace, "pipeline.config")
  assert source_game_dir and output_dir and workspace and executable and config_root and pipeline_config
  if not (source_game_dir / "config.yaml").is_file():
    raise SystemExit(f"source game config is missing: {source_game_dir / 'config.yaml'}")
  if not executable.is_file():
    raise SystemExit(f"pipeline executable is missing: {executable}")
  ffmpeg = shutil.which("ffmpeg")
  if ffmpeg is None:
    raise SystemExit("ffmpeg is required to convert calibration panoramas to PNG")
  output_dir.mkdir(parents=True, exist_ok=False)
  logs_dir = output_dir / "logs"
  evidence_dir = output_dir / "evidence"
  configs_dir = output_dir / "effective-configs"
  for directory in (logs_dir, evidence_dir, configs_dir):
    directory.mkdir()
  shutil.copy2(cli.config, output_dir / "matrix-config.yaml")
  source_config_snapshot = output_dir / "source-game-config.yaml"
  shutil.copy2(source_game_dir / "config.yaml", source_config_snapshot)

  runner_args = SimpleNamespace(
      workspace=workspace,
      pipeline_app=executable,
      config_root=config_root,
      pipeline_config=pipeline_config,
      control_points=int(config.get("defaults", {}).get("control_points", 900)),
      frame_count=int(config.get("defaults", {}).get("frame_count", 4)),
      timeout=as_positive_number(pipeline.get("timeout_seconds", 360), "pipeline.timeout_seconds"),
      max_live_canvas_dimension=as_integer(
          pipeline.get("max_live_canvas_dimension", 4096), "pipeline.max_live_canvas_dimension"
      ),
      resource_check_interval=as_positive_number(resources.get("check_interval_seconds", 5), "resources.check_interval_seconds"),
      min_available_memory_gib=float(resources.get("min_available_memory_gib", 0)),
      min_gpu_free_memory_gib=float(resources.get("min_gpu_free_memory_gib", 0)),
      gpu_launch_reserve_gib=float(resources.get("gpu_launch_reserve_gib", 0)),
      min_tmp_free_gib=float(resources.get("min_tmp_free_gib", 0)),
      headroom_wait_timeout=float(resources.get("headroom_wait_timeout_seconds", 120)),
  )
  resource_values = (
      runner_args.min_available_memory_gib,
      runner_args.min_gpu_free_memory_gib,
      runner_args.gpu_launch_reserve_gib,
      runner_args.min_tmp_free_gib,
      runner_args.headroom_wait_timeout,
  )
  if any(not math.isfinite(value) or value < 0 for value in resource_values):
    raise ValueError("resource thresholds and headroom wait timeout must be finite and non-negative")

  lock_path = Path(os.getenv("TMPDIR", "/tmp")) / "hstream-stitching-calibration-matrix.lock"
  failures = 0
  with lock_path.open("a+", encoding="utf-8") as lock_stream, (output_dir / "manifest.csv").open(
      "w", encoding="utf-8", newline=""
  ) as manifest:
    try:
      fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError as exception:
      raise SystemExit(f"another stitching calibration matrix owns {lock_path}") from exception
    writer = csv.DictWriter(manifest, fieldnames=MANIFEST_FIELDS)
    writer.writeheader()
    for sequence, state in selected:
      stem = output_stem(sequence, state)
      log_path = logs_dir / f"{stem}.log"
      effective_path = configs_dir / f"{stem}.yaml"
      with effective_path.open("w", encoding="utf-8") as stream:
        yaml.safe_dump(effective_case_config(state), stream, sort_keys=False)
      print(f"START {sequence:03d}/{selected[-1][0]:03d} {state['case']}", flush=True)
      calibration_matrix.wait_for_resource_headroom(runner_args)
      work_root, game_id = calibration_matrix.create_isolated_game(source_game_dir, source_config_snapshot)
      result: dict[str, object] | None = None
      png_path = output_dir / f"{stem}.png"
      try:
        result = calibration_matrix.run_state(runner_args, work_root, game_id, state, sequence, log_path)
        if result["outcome"] == "pass":
          convert_panorama(work_root / game_id / "panorama.tif", png_path, ffmpeg)
          for source_name, suffix in (("autooptimiser_out.pto", ".pto"), ("stitching_canvas_provenance", ".provenance.txt")):
            source = work_root / game_id / source_name
            if source.is_file():
              shutil.copy2(source, evidence_dir / f"{stem}{suffix}")
      except Exception as exception:
        if result is None:
          result = {"outcome": "fail", "duration_seconds": "", "return_code": "", "first_failure": str(exception)}
        else:
          result["outcome"] = "fail"
          result["first_failure"] = str(exception)
        png_path.unlink(missing_ok=True)
      finally:
        keep_work = result is not None and result["outcome"] != "pass" and cli.keep_failed_work
        if not keep_work:
          calibration_matrix.remove_work_root(work_root)
      assert result is not None
      failures += result["outcome"] != "pass"
      writer.writerow(
          {
              "sequence": sequence,
              "label": state["label"],
              "projection": state["projection"],
              "parameters": calibration_matrix.parameter_text(tuple(state["parameters"])),
              "auto_fov": state["auto_fov"],
              "horizontal_fov": state["horizontal_fov"],
              "auto_canvas": state["auto_canvas"],
              "auto_crop": state["auto_crop"],
              "png": str(png_path) if png_path.is_file() else "",
              "effective_config": str(effective_path),
              "log": str(log_path),
              "outcome": result["outcome"],
              "duration_seconds": result.get("duration_seconds", ""),
              "return_code": result.get("return_code", ""),
              "first_failure": result.get("first_failure", ""),
              "work_directory": str(work_root) if keep_work else "",
          }
      )
      manifest.flush()
      os.fsync(manifest.fileno())
      print(f"{str(result['outcome']).upper()} {sequence:03d} {state['case']}", flush=True)
  print(f"Captured {len(selected) - failures}/{len(selected)} PNG frames in {output_dir}")
  return 1 if failures else 0


if __name__ == "__main__":
  try:
    raise SystemExit(main())
  except (OSError, ValueError, yaml.YAMLError) as exception:
    print(f"error: {exception}", file=sys.stderr)
    raise SystemExit(2) from exception
