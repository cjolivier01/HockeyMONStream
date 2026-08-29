#!/usr/bin/env python3
"""Run real stitching-only calibration permutations against one game's videos."""

from __future__ import annotations

import argparse
import csv
import fcntl
import itertools
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time

import yaml


PARAMETERLESS_PROJECTIONS = (
    "rectilinear",
    "cylindrical",
    "equirectangular",
    "full-frame-fisheye",
    "stereographic",
    "mercator",
    "transverse-mercator",
    "sinusoidal",
    "lambert-cylindrical-equal-area",
    "lambert-azimuthal-equal-area",
    "miller-cylindrical",
    "panini",
    "architectural",
    "orthographic",
    "equisolid",
    "equirectangular-panini",
    "thoby",
    "hammer-aitoff",
)

PARAMETER_VALUES = {
    "albers-equal-area-conic": ((-90, 0, 90), (-90, 60, 90)),
    "biplane": ((1, 45, 179), (0, 1)),
    "triplane": ((1, 60, 120),),
    "general-panini": ((0, 100, 150), (-100, 0, 100), (-100, 0, 100)),
}


def projection_framing(state: dict[str, object]) -> dict[str, object]:
  return {
      "auto_fov": bool(state.get("auto_fov", False)),
      "horizontal_fov": float(state.get("horizontal_fov", 180)),
      "auto_canvas": bool(state.get("auto_canvas", True)),
      "auto_crop": bool(state.get("auto_crop", False)),
  }


def framing_states() -> list[dict[str, object]]:
  common = {
      "backend": "nona",
      "parameters": (100, 0, 0),
      "control_points": 900,
      "frame_count": 4,
      "stitch_frame_time": "00:00:00",
      "max_output_width": 2048,
      "horizontal_fov": 180,
  }
  return [
      {
          **common,
          "case": "general-panini-fixed-180-auto-canvas-full-canvas-crop",
          "projection": "general-panini",
          "auto_fov": False,
          "auto_canvas": True,
          "auto_crop": False,
      },
      {
          **common,
          "case": "general-panini-fixed-185-auto-canvas-full-canvas-crop",
          "projection": "general-panini",
          "horizontal_fov": 185,
          "auto_fov": False,
          "auto_canvas": True,
          "auto_crop": False,
      },
      {
          **common,
          "case": "general-panini-auto-fov-auto-canvas-full-canvas-crop",
          "projection": "general-panini",
          "auto_fov": True,
          "auto_canvas": True,
          "auto_crop": False,
      },
      {
          **common,
          "case": "general-panini-fixed-180-retained-canvas-full-canvas-crop",
          "projection": "general-panini",
          "auto_fov": False,
          "auto_canvas": False,
          "auto_crop": False,
      },
      {
          **common,
          "case": "general-panini-fixed-180-auto-canvas-auto-crop",
          "projection": "general-panini",
          "auto_fov": False,
          "auto_canvas": True,
          "auto_crop": True,
      },
      {
          **common,
          "case": "general-panini-auto-fov-retained-canvas-auto-crop",
          "projection": "general-panini",
          "auto_fov": True,
          "auto_canvas": False,
          "auto_crop": True,
      },
      {
          **common,
          "case": "cylindrical-fixed-180-auto-canvas-full-canvas-crop",
          "projection": "cylindrical",
          "parameters": (),
          "auto_fov": False,
          "auto_canvas": True,
          "auto_crop": False,
      },
      {
          **common,
          "case": "stereographic-auto-fov-auto-canvas-auto-crop",
          "projection": "stereographic",
          "parameters": (),
          "auto_fov": True,
          "auto_canvas": True,
          "auto_crop": True,
      },
  ]


def parameter_text(parameters: tuple[float, ...]) -> str:
  return "none" if not parameters else ",".join(f"{value:g}" for value in parameters)


def state_id(
    backend: str,
    projection: str,
    parameters: tuple[float, ...],
    framing: dict[str, object] | None = None,
) -> str:
  suffix = parameter_text(parameters).replace("-", "m").replace(",", "_")
  identifier = f"{backend}__{projection}__{suffix}"
  if framing is None:
    return identifier
  fov = "auto-fov" if framing["auto_fov"] else f"fixed-{float(framing['horizontal_fov']):g}"
  canvas = "auto-canvas" if framing["auto_canvas"] else "retained-canvas"
  crop = "auto-crop" if framing["auto_crop"] else "full-canvas-crop"
  return f"{identifier}__{fov}__{canvas}__{crop}"


def projection_states() -> list[dict[str, object]]:
  states: list[dict[str, object]] = []
  for projection in PARAMETERLESS_PROJECTIONS:
    states.append(
        {
            "backend": "nona",
            "projection": projection,
            "parameters": (),
            "max_output_width": 4096,
            "auto_fov": True,
        }
    )
  for projection, dimensions in PARAMETER_VALUES.items():
    for parameters in itertools.product(*dimensions):
      states.append(
          {
              "backend": "nona",
              "projection": projection,
              "parameters": parameters,
              "max_output_width": 4096,
              "auto_fov": True,
          }
      )
  defaults = {
      "albers-equal-area-conic": (0, 60),
      "biplane": (45, 0),
      "triplane": (60,),
      "general-panini": (100, 0, 0),
  }
  fractional = {
      "albers-equal-area-conic": (12.5, 47.25),
      "biplane": (45.5, 1),
      "triplane": (60.5,),
      "general-panini": (100.5, -12.5, 12.5),
  }
  for projection, parameters in defaults.items():
    states.append(
        {
            "backend": "nona",
            "projection": projection,
            "parameters": parameters,
            "omit_parameters": True,
            "case": f"nona__{projection}__default-omitted",
            "max_output_width": 4096,
            "auto_fov": True,
        }
    )
  for projection, parameters in fractional.items():
    states.append(
        {
            "backend": "nona",
            "projection": projection,
            "parameters": parameters,
            "case": f"nona__{projection}__fractional",
            "max_output_width": 4096,
            "auto_fov": True,
        }
    )
  states.extend(
      {
          "backend": backend,
          "projection": "rectilinear",
          "parameters": (),
          "max_output_width": 4096,
          "horizontal_fov": 179,
      }
      for backend in ("opencv-magsac", "opencv-affine-ransac")
  )
  return states


def boundary_states() -> list[dict[str, object]]:
  base = {
      "backend": "nona",
      "projection": "general-panini",
      "parameters": (100, 0, 0),
      "control_points": 1500,
      "frame_count": 4,
      "stitch_frame_time": "00:00:00",
      "max_output_width": 4096,
  }
  states: list[dict[str, object]] = [{**base, "case": "boundary-default"}]
  for value in (20, 5000):
    states.append({**base, "case": f"control-points-{value}", "control_points": value})
  for value in (1, 16, 64):
    states.append({**base, "case": f"frame-count-{value}", "frame_count": value})
  for value in ("00:15:00", "00:15:00.500", "00:57:00"):
    states.append({**base, "case": f"stitch-time-{value.replace(':', '')}", "stitch_frame_time": value})
  for value in (0, 1, 2_147_483_647):
    states.append({**base, "case": f"max-width-{value}", "max_output_width": value})
  for backend in ("opencv-magsac", "opencv-affine-ransac"):
    for value in (0, 1, 2_147_483_647):
      states.append(
          {
              **base,
              "case": f"{backend}-max-width-{value}",
              "backend": backend,
              "projection": "rectilinear",
              "parameters": (),
              "max_output_width": value,
              "horizontal_fov": 179,
          }
      )
  for value in (0, 1, 2_147_483_647):
    states.append(
        {
            **base,
            "case": f"nona-rectilinear-max-width-{value}",
            "projection": "rectilinear",
            "parameters": (),
            "max_output_width": value,
            "horizontal_fov": 179,
        }
    )
  states.append({**base, "case": "stress-control-points-5000-frames-16", "control_points": 5000, "frame_count": 16})
  states.append(
      {
          **base,
          "case": "stress-time-005700-frames-16",
          "frame_count": 16,
          "stitch_frame_time": "00:57:00",
      }
  )
  return states


def configure_game(
    config_path: Path,
    state: dict[str, object],
    invalidation_id: str,
    control_points: int,
    frame_count: int,
) -> None:
  with config_path.open("r", encoding="utf-8") as stream:
    config = yaml.safe_load(stream) or {}
  backend = str(state["backend"])
  projection = str(state["projection"])
  parameters = tuple(state["parameters"])
  stitch_frame_time = str(state.get("stitch_frame_time", "00:00:00"))
  max_output_width = int(state.get("max_output_width", 0))
  framing = projection_framing(state)
  calibration = config.setdefault("hstream_ui", {}).setdefault("stitching_calibration", {})
  calibration.update(
      {
          "control_points": control_points,
          "frame_count": frame_count,
          "status": "pending",
          "rink_mask_status": "pending",
          "stale_from": "input",
          "artifacts_invalidated": False,
          "invalidation_id": invalidation_id,
          "backend_generation": {
              "invalidation_id": invalidation_id,
              "control_point_matcher": "superpoint-lightglue",
              "mapping_backend": backend,
              "projection": projection,
              "run_autooptimizer": backend == "nona",
              "projection_parameters": list(parameters),
              "projection_framing": framing,
          },
      }
  )
  stitching = config.setdefault("stitching", {})
  stitching.update(
      {
          "control_point_matcher": "superpoint-lightglue",
          "mapping_backend": backend,
          "projection": projection,
          "run_autooptimizer": backend == "nona",
          "calibration_frame_count": frame_count,
      }
  )
  stitching["projection_framing"] = framing
  if stitch_frame_time == "00:00:00":
    stitching.pop("stitch_frame_time", None)
  else:
    stitching["stitch_frame_time"] = stitch_frame_time
  if max_output_width == 0:
    stitching.pop("max_output_width", None)
  else:
    stitching["max_output_width"] = max_output_width
  if parameters:
    parameter_map = stitching.setdefault("projection_parameters", {})
    if state.get("omit_parameters", False):
      parameter_map.pop(projection, None)
    else:
      parameter_map[projection] = list(parameters)
  temporary = config_path.with_name(f".{config_path.name}.matrix-tmp")
  with temporary.open("w", encoding="utf-8") as stream:
    yaml.safe_dump(config, stream, sort_keys=False)
    stream.flush()
    os.fsync(stream.fileno())
  os.replace(temporary, config_path)


def interrupt_process(process: subprocess.Popen[str]) -> threading.Timer | None:
  if process.poll() is not None:
    return None
  try:
    os.killpg(process.pid, signal.SIGINT)
  except ProcessLookupError:
    return None

  def force_kill_process_group() -> None:
    if process.poll() is not None:
      return
    try:
      os.killpg(process.pid, signal.SIGKILL)
    except ProcessLookupError:
      pass

  force_kill = threading.Timer(10, force_kill_process_group)
  force_kill.daemon = True
  force_kill.start()
  return force_kill


def read_canvas_provenance(path: Path) -> dict[str, str]:
  values: dict[str, str] = {}
  with path.open("r", encoding="utf-8") as stream:
    for line in stream:
      key, separator, value = line.rstrip("\n").partition("=")
      if not separator or not key or key in values:
        raise ValueError(f"invalid or duplicate provenance entry: {line.rstrip()}")
      values[key] = value
  return values


def read_pto_panorama(path: Path) -> dict[str, str]:
  panorama_line = ""
  with path.open("r", encoding="utf-8") as stream:
    for line in stream:
      if line.startswith("p "):
        if panorama_line:
          raise ValueError("PTO contains multiple panorama lines")
        panorama_line = line.rstrip("\n")
  if not panorama_line:
    raise ValueError("PTO has no panorama line")

  patterns = {
      "f": r"(?:^|\s)f(-?[0-9]+)(?=\s|$)",
      "v": r"(?:^|\s)v([-+0-9.eE]+)(?=\s|$)",
      "w": r"(?:^|\s)w([0-9]+)(?=\s|$)",
      "h": r"(?:^|\s)h([0-9]+)(?=\s|$)",
      "S": r"(?:^|\s)S([0-9]+,[0-9]+,[0-9]+,[0-9]+)(?=\s|$)",
  }
  values: dict[str, str] = {}
  for key, pattern in patterns.items():
    match = re.search(pattern, panorama_line)
    if match is None:
      if key == "S":
        values[key] = "none"
        continue
      raise ValueError(f"PTO panorama line has no {key} field")
    values[key] = match.group(1)
  fov = float(values["v"])
  if not math.isfinite(fov) or fov <= 0 or int(values["w"]) <= 0 or int(values["h"]) <= 0:
    raise ValueError("PTO panorama line has invalid FOV or canvas dimensions")
  return values


def verify_canvas_provenance(
    game_dir: Path,
    backend: str,
    projection: str,
    parameters: tuple[float, ...],
    max_output_width: int,
    max_canvas_dimension: int,
    framing: dict[str, object],
) -> tuple[dict[str, str], str]:
  provenance: dict[str, str] = {}
  try:
    provenance = read_canvas_provenance(game_dir / "stitching_canvas_provenance")
    expected_parameters = parameter_text(parameters)
    actual_width = int(provenance["canvas-width"])
    actual_height = int(provenance["canvas-height"])
    actual_max_width = int(provenance["max-output-width"])
    actual_max_dimension = int(provenance["max-canvas-dimension"])
    if provenance["version"] != "5":
      raise ValueError(f"published provenance version is {provenance['version']}, expected 5")
    if provenance["mapping-backend"] != backend:
      raise ValueError(f"published backend is {provenance['mapping-backend']}, expected {backend}")
    if provenance["projection"] != projection:
      raise ValueError(f"published projection is {provenance['projection']}, expected {projection}")
    if provenance["projection-parameters"] != expected_parameters:
      raise ValueError(
          f"published projection parameters are {provenance['projection-parameters']}, expected {expected_parameters}"
      )
    if actual_max_width != max_output_width:
      raise ValueError(f"published max width is {actual_max_width}, expected {max_output_width}")
    if actual_max_dimension != max_canvas_dimension:
      raise ValueError(f"published max canvas dimension is {actual_max_dimension}, expected {max_canvas_dimension}")
    if actual_width <= 0 or actual_height <= 0:
      raise ValueError(f"published canvas is not positive: {actual_width}x{actual_height}")
    if max_output_width > 0 and actual_width > max_output_width:
      raise ValueError(f"published canvas width {actual_width} exceeds {max_output_width}")
    if max(actual_width, actual_height) > max_canvas_dimension:
      raise ValueError(
          f"published canvas {actual_width}x{actual_height} exceeds max dimension {max_canvas_dimension}"
      )
    expected_framing = {
        "projection-auto-fov": "1" if framing["auto_fov"] else "0",
        "projection-horizontal-fov": f"{float(framing['horizontal_fov']):g}",
        "projection-auto-canvas": "1" if framing["auto_canvas"] else "0",
        "projection-auto-crop": "1" if framing["auto_crop"] else "0",
    }
    for key, expected in expected_framing.items():
      actual = provenance[key]
      if key == "projection-horizontal-fov":
        if float(actual) != float(expected):
          raise ValueError(f"published {key} is {actual}, expected {expected}")
      elif actual != expected:
        raise ValueError(f"published {key} is {actual}, expected {expected}")
    return provenance, ""
  except (KeyError, OSError, ValueError) as exception:
    return provenance, f"published stitching provenance verification failed: {exception}"


def run_state(
    args: argparse.Namespace,
    work_root: Path,
    game_id: str,
    state: dict[str, object],
    sequence: int,
    log_path: Path,
) -> dict[str, object]:
  backend = str(state["backend"])
  projection = str(state["projection"])
  parameters = tuple(state["parameters"])
  control_points = int(state.get("control_points", args.control_points))
  frame_count = int(state.get("frame_count", args.frame_count))
  max_output_width = int(state.get("max_output_width", 0))
  framing = projection_framing(state)
  invalidation_id = f"matrix-{sequence:03d}-{state_id(backend, projection, parameters, framing)}"
  configure_game(work_root / game_id / "config.yaml", state, invalidation_id, control_points, frame_count)
  environment = os.environ.copy()
  environment.update(
      {
          "HM_GAME_DIR": str(work_root),
          "HM_CONFIG_ROOT": str(args.config_root),
          "HM_MAX_CONTROL_POINTS": str(control_points),
          "HM_STITCH_CALIBRATION_FRAME_COUNT": str(frame_count),
          "HM_MAX_LIVE_STITCH_EGL_DIMENSION": str(args.max_live_canvas_dimension),
          "HSTREAM_CALIBRATION_PENDING": "1",
          "HSTREAM_CALIBRATION_START_STAGE": "input",
          "HSTREAM_CALIBRATION_INVALIDATION_ID": invalidation_id,
          "HSTREAM_RENDER_AUDIO_MUTED": "1",
          "USE_NEW_NVSTREAMMUX": "yes",
      }
  )
  command = [
      str(args.pipeline_app),
      "-g",
      game_id,
      "--enable-sources=URI-MULTIPLE",
      "--force-reconfigure",
      f"--clean-expected-invalidation-id={invalidation_id}",
      "--stitching-calibration-only",
      "-c",
      str(args.pipeline_config),
      "--enable-sinks=FAKE",
      "--options=pipeline.streammux.batch-size=2,pipeline.streammux.sync-inputs=0,"
      "pipeline.streammux.batched-push-timeout=2147483647,"
      "pipeline.streammux.frame-num-reset-on-stream-reset=0,"
      "pipeline.streammux.frame-num-reset-on-eos=0,pipeline.hmstitcher.show=0",
      f"--options=pipeline.hmstitcher.calibration-frame-count={frame_count}",
      "-t=1",
  ]
  started = time.monotonic()
  completed = False
  playback_restarted = False
  app_success = False
  first_failure = ""
  timed_out = threading.Event()
  resource_watchdog_triggered = threading.Event()
  resource_failure: list[str] = []
  process_finished = threading.Event()
  termination_lock = threading.Lock()
  process = subprocess.Popen(
      command,
      cwd=args.workspace,
      env=environment,
      stdout=subprocess.PIPE,
      stderr=subprocess.STDOUT,
      text=True,
      errors="replace",
      start_new_session=True,
      bufsize=1,
  )

  def terminate_running(marker: threading.Event | None = None, failure: str = "") -> bool:
    with termination_lock:
      if process_finished.is_set():
        return False
      if process.poll() is not None:
        process_finished.set()
        return False
      # Mark a timeout/resource failure only after SIGINT was actually sent.
      # The process can exit between poll() above and killpg(); treating that
      # ProcessLookupError window as a timeout makes successful boundary-time
      # exits nondeterministically fail the CSV result.
      if interrupt_process(process) is None:
        if process.poll() is not None:
          process_finished.set()
        return False
      if marker is not None:
        marker.set()
      if failure:
        resource_failure.append(failure)
      return True

  def timeout_process() -> None:
    if not process_finished.wait(args.timeout):
      terminate_running(timed_out)

  def watch_resource_headroom() -> None:
    while not process_finished.wait(args.resource_check_interval):
      failure = resource_headroom_failure(args)
      if failure and terminate_running(resource_watchdog_triggered, failure):
        return

  timeout_thread = threading.Thread(target=timeout_process, name="matrix-timeout", daemon=True)
  resource_thread = threading.Thread(target=watch_resource_headroom, name="matrix-resource-watchdog", daemon=True)
  timeout_thread.start()
  resource_thread.start()
  try:
    with log_path.open("w", encoding="utf-8") as log:
      assert process.stdout is not None
      for line in process.stdout:
        log.write(line)
        if "HSTREAM_CALIBRATION stage=calibration status=complete" in line:
          completed = True
        if "HSTREAM_CALIBRATION stage=playback-restart status=complete" in line:
          playback_restarted = True
        if "App run successful" in line:
          app_success = True
        if "HSTREAM_CALIBRATION" in line and "status=failed" in line and not first_failure:
          first_failure = line.strip()
          terminate_running()
    return_code = process.wait()
    with termination_lock:
      process_finished.set()
  except BaseException:
    terminate_running()
    try:
      process.wait(timeout=12)
    except subprocess.TimeoutExpired:
      try:
        os.killpg(process.pid, signal.SIGKILL)
      except ProcessLookupError:
        pass
      process.wait()
    raise
  finally:
    with termination_lock:
      process_finished.set()
    if process.stdout is not None:
      process.stdout.close()
    timeout_thread.join(timeout=1)
    resource_thread.join(timeout=12)
  duration = time.monotonic() - started
  provenance: dict[str, str] = {}
  pto: dict[str, str] = {}
  if completed:
    provenance, provenance_failure = verify_canvas_provenance(
        work_root / game_id,
        backend,
        projection,
        parameters,
        max_output_width,
        args.max_live_canvas_dimension,
        framing,
    )
    if provenance_failure and not first_failure:
      first_failure = provenance_failure
    try:
      pto = read_pto_panorama(work_root / game_id / "autooptimiser_out.pto")
    except (OSError, ValueError) as exception:
      if not first_failure:
        first_failure = f"published PTO evidence verification failed: {exception}"
  if timed_out.is_set() and not first_failure:
    first_failure = f"timed out after {args.timeout:g} seconds"
  if resource_failure and not first_failure:
    first_failure = resource_failure[0]
  outcome = (
      "pass"
      if completed
      and playback_restarted
      and app_success
      and return_code == 0
      and not timed_out.is_set()
      and not resource_watchdog_triggered.is_set()
      and not first_failure
      else "fail"
  )
  return {
      "sequence": sequence,
      "case": state.get("case", state_id(backend, projection, parameters, framing)),
      "backend": backend,
      "projection": projection,
      "parameters": parameter_text(parameters),
      "control_points": control_points,
      "frame_count": frame_count,
      "stitch_frame_time": state.get("stitch_frame_time", "00:00:00"),
      "max_output_width": max_output_width,
      "configured_live_dimension_cap": args.max_live_canvas_dimension,
      **framing,
      "actual_backend": provenance.get("mapping-backend", ""),
      "actual_projection": provenance.get("projection", ""),
      "actual_parameters": provenance.get("projection-parameters", ""),
      "actual_max_output_width": provenance.get("max-output-width", ""),
      "actual_live_dimension_cap": provenance.get("max-canvas-dimension", ""),
      "actual_live_dimension_cap_applied": provenance.get("max-canvas-dimension-applied", ""),
      "actual_projection_auto_fov": provenance.get("projection-auto-fov", ""),
      "actual_projection_horizontal_fov": provenance.get("projection-horizontal-fov", ""),
      "actual_projection_auto_canvas": provenance.get("projection-auto-canvas", ""),
      "actual_projection_auto_crop": provenance.get("projection-auto-crop", ""),
      "canvas_width": provenance.get("canvas-width", ""),
      "canvas_height": provenance.get("canvas-height", ""),
      "pto_f": pto.get("f", ""),
      "pto_v": pto.get("v", ""),
      "pto_w": pto.get("w", ""),
      "pto_h": pto.get("h", ""),
      "pto_S": pto.get("S", ""),
      "actual_fov": pto.get("v", ""),
      "actual_fov_source": "autooptimiser_out.pto panorama v" if pto else "",
      "calibration_complete": completed,
      "playback_restart_complete": playback_restarted,
      "app_run_success": app_success,
      "timed_out": timed_out.is_set(),
      "resource_watchdog_triggered": resource_watchdog_triggered.is_set(),
      "resource_failure": resource_failure[0] if resource_failure else "",
      "outcome": outcome,
      "duration_seconds": f"{duration:.3f}",
      "return_code": return_code,
      "first_failure": first_failure,
      "log": str(log_path),
      "before_sample": "",
      "after_sample": "",
      "sample_failure": "",
  }


def parse_args() -> argparse.Namespace:
  parser = argparse.ArgumentParser()
  parser.add_argument("--source-game-dir", type=Path, required=True)
  parser.add_argument("--workspace", type=Path, default=Path.cwd())
  parser.add_argument("--pipeline-app", type=Path)
  parser.add_argument("--config-root", type=Path)
  parser.add_argument("--pipeline-config", type=Path)
  parser.add_argument("--output-dir", type=Path)
  parser.add_argument(
      "--mode", choices=("framing", "projections", "boundaries", "all"), default="framing"
  )
  parser.add_argument("--control-points", type=int, default=900)
  parser.add_argument("--frame-count", type=int, default=4)
  parser.add_argument(
      "--timeout",
      type=float,
      help="Per-case timeout in seconds (default: 360 for framing/all, 180 otherwise)",
  )
  parser.add_argument("--start-at", type=int, default=1)
  parser.add_argument("--limit", type=int)
  parser.add_argument("--min-available-memory-gib", type=float, default=12)
  parser.add_argument("--min-gpu-free-memory-gib", type=float, default=12)
  parser.add_argument(
      "--gpu-launch-reserve-gib",
      type=float,
      default=8,
      help="Additional free GPU memory required before launching a case (watchdog reserve remains unchanged)",
  )
  parser.add_argument("--min-tmp-free-gib", type=float, default=12)
  parser.add_argument("--resource-check-interval", type=float, default=5)
  parser.add_argument("--headroom-wait-timeout", type=float, default=120)
  parser.add_argument("--max-live-canvas-dimension", type=int, default=2048)
  parser.add_argument(
      "--capture-samples",
      action="store_true",
      help="Create bounded JPEG input/stitched samples and retain their PTO/provenance evidence",
  )
  parser.add_argument("--sample-width", type=int, default=1600)
  parser.add_argument("--keep-failed-work", action="store_true")
  args = parser.parse_args()
  args.workspace = args.workspace.resolve()
  args.source_game_dir = args.source_game_dir.resolve()
  args.pipeline_app = (args.pipeline_app or args.workspace / "bazel-bin/src/apps/pipeline-app/pipeline-app").resolve()
  args.config_root = (args.config_root or args.workspace / "configs").resolve()
  args.pipeline_config = (args.pipeline_config or args.config_root / "ds_hockey_app_config.yaml").resolve()
  if args.timeout is None:
    args.timeout = 360.0 if args.mode in ("framing", "all") else 180.0
  if args.timeout <= 0 or args.resource_check_interval <= 0 or args.headroom_wait_timeout < 0:
    parser.error("timeouts must be positive and --headroom-wait-timeout must be non-negative")
  if args.start_at <= 0 or (args.limit is not None and args.limit <= 0):
    parser.error("--start-at and --limit must be positive")
  if args.max_live_canvas_dimension <= 0 or args.sample_width < 320 or args.sample_width > 4096:
    parser.error("--max-live-canvas-dimension must be positive and --sample-width must be between 320 and 4096")
  if min(
      args.min_available_memory_gib,
      args.min_gpu_free_memory_gib,
      args.gpu_launch_reserve_gib,
      args.min_tmp_free_gib,
  ) < 0:
    parser.error("resource headroom thresholds must be non-negative")
  if args.output_dir is None:
    args.output_dir = Path(tempfile.mkdtemp(prefix="hstream-stitching-matrix-results-"))
  else:
    args.output_dir = args.output_dir.resolve()
    args.output_dir.mkdir(parents=True, exist_ok=False)
  return args


def available_memory_bytes() -> int:
  with Path("/proc/meminfo").open("r", encoding="utf-8") as stream:
    for line in stream:
      if line.startswith("MemAvailable:"):
        return int(line.split()[1]) * 1024
  raise RuntimeError("/proc/meminfo does not report MemAvailable")


def resource_headroom_failure(args: argparse.Namespace, before_launch: bool = False) -> str:
  gib = 1024**3
  try:
    if args.min_available_memory_gib > 0:
      available_memory = available_memory_bytes()
      if available_memory < args.min_available_memory_gib * gib:
        return (
            f"only {available_memory / gib:.1f} GiB memory available; "
            f"matrix requires {args.min_available_memory_gib:g} GiB"
        )
    if args.min_tmp_free_gib > 0:
      available_tmp = shutil.disk_usage(tempfile.gettempdir()).free
      if available_tmp < args.min_tmp_free_gib * gib:
        return (
            f"only {available_tmp / gib:.1f} GiB free in {tempfile.gettempdir()}; "
            f"matrix requires {args.min_tmp_free_gib:g} GiB"
        )
    if args.min_gpu_free_memory_gib > 0:
      gpu_query = subprocess.run(
          ["nvidia-smi", "--query-gpu=memory.free", "--format=csv,noheader,nounits"],
          check=True,
          capture_output=True,
          text=True,
          timeout=10,
      )
      gpu_free_values = [int(value.strip()) * 1024**2 for value in gpu_query.stdout.splitlines() if value.strip()]
      if not gpu_free_values:
        return "nvidia-smi did not report free GPU memory"
      required_gpu_gib = args.min_gpu_free_memory_gib + (args.gpu_launch_reserve_gib if before_launch else 0)
      if min(gpu_free_values) < required_gpu_gib * gib:
        return (
            f"only {min(gpu_free_values) / gib:.1f} GiB GPU memory available; "
            f"matrix requires {required_gpu_gib:g} GiB"
            + (" before launching a case" if before_launch else "")
        )
  except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as exception:
    return f"unable to verify resource headroom: {exception}"
  return ""


def wait_for_resource_headroom(args: argparse.Namespace) -> None:
  deadline = time.monotonic() + args.headroom_wait_timeout
  while True:
    failure = resource_headroom_failure(args, before_launch=True)
    if not failure:
      return
    remaining = deadline - time.monotonic()
    if remaining <= 0:
      raise RuntimeError(
          f"resource headroom did not recover within {args.headroom_wait_timeout:g} seconds: {failure}"
      )
    time.sleep(min(args.resource_check_interval, remaining))


def create_isolated_game(source_game_dir: Path, source_config: Path) -> tuple[Path, str]:
  work_root = Path(tempfile.mkdtemp(prefix="hstream-stitching-matrix-work-"))
  game_id = f"{source_game_dir.name}-matrix"
  game_dir = work_root / game_id
  try:
    game_dir.mkdir()
    shutil.copy2(source_config, game_dir / "config.yaml")
    for name in ("left.mp4", "right.mp4", "left_calibration.json"):
      source = source_game_dir / name
      if source.exists():
        (game_dir / name).symlink_to(source.resolve())
  except BaseException:
    shutil.rmtree(work_root, ignore_errors=True)
    raise
  return work_root, game_id


def remove_work_root(work_root: Path) -> None:
  shutil.rmtree(work_root)
  if work_root.exists():
    raise RuntimeError(f"matrix work directory still exists after cleanup: {work_root}")


def capture_case_samples(
    game_dir: Path,
    output_dir: Path,
    sequence: int,
    case: str,
    sample_width: int,
) -> tuple[str, str, str]:
  ffmpeg = shutil.which("ffmpeg")
  if ffmpeg is None:
    return "", "", "ffmpeg is unavailable; could not create inspectable JPEG samples"
  inputs = (game_dir / "left.png", game_dir / "right.png")
  panorama = game_dir / "panorama.tif"
  missing = [str(path) for path in (*inputs, panorama) if not path.is_file()]
  if missing:
    return "", "", f"sample source artifact(s) missing: {', '.join(missing)}"
  samples_dir = output_dir / "samples"
  samples_dir.mkdir(parents=True, exist_ok=True)
  before = samples_dir / "input-pair.jpg"
  safe_case = re.sub(r"[^A-Za-z0-9_.-]+", "-", case).strip(".-") or "case"
  after = samples_dir / f"{sequence:03d}-{safe_case}-stitched.jpg"
  half_width = max(2, (sample_width // 2) // 2 * 2)
  try:
    if not before.is_file():
      subprocess.run(
          [
              ffmpeg,
              "-v",
              "error",
              "-y",
              "-threads",
              "2",
              "-i",
              str(inputs[0]),
              "-i",
              str(inputs[1]),
              "-filter_complex",
              f"[0:v]scale={half_width}:-2[left];[1:v]scale={half_width}:-2[right];"
              "[left][right]hstack=inputs=2,format=yuvj420p[out]",
              "-map",
              "[out]",
              "-frames:v",
              "1",
              "-q:v",
              "3",
              str(before),
          ],
          check=True,
          timeout=120,
      )
    subprocess.run(
        [
            ffmpeg,
            "-v",
            "error",
            "-y",
            "-threads",
            "2",
            "-i",
            str(panorama),
            "-vf",
            f"scale={sample_width}:-2:force_original_aspect_ratio=decrease,format=yuvj420p",
            "-frames:v",
            "1",
            "-q:v",
            "3",
            str(after),
        ],
        check=True,
        timeout=120,
    )
    for source_name, suffix in (
        ("autooptimiser_out.pto", "project.pto"),
        ("stitching_canvas_provenance", "provenance.txt"),
    ):
      source = game_dir / source_name
      if source.is_file():
        shutil.copy2(source, samples_dir / f"{sequence:03d}-{safe_case}-{suffix}")
  except (OSError, subprocess.SubprocessError) as exception:
    after.unlink(missing_ok=True)
    return str(before) if before.is_file() else "", "", f"could not create JPEG samples: {exception}"
  return str(before), str(after), ""


def main() -> int:
  args = parse_args()
  source_config = args.source_game_dir / "config.yaml"
  if not source_config.is_file() or not args.pipeline_app.is_file():
    raise SystemExit("source config or pipeline-app is missing")
  lock_path = Path(tempfile.gettempdir()) / "hstream-stitching-calibration-matrix.lock"
  lock_stream = lock_path.open("a+", encoding="utf-8")
  try:
    fcntl.flock(lock_stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
  except BlockingIOError as exception:
    raise SystemExit(f"another stitching calibration matrix owns {lock_path}") from exception
  source_config_snapshot = args.output_dir / "source-config.yaml"
  shutil.copy2(source_config, source_config_snapshot)
  states: list[dict[str, object]] = []
  if args.mode in ("framing", "all"):
    states.extend(framing_states())
  if args.mode in ("projections", "all"):
    states.extend(projection_states())
  if args.mode in ("boundaries", "all"):
    states.extend(boundary_states())
  states = states[args.start_at - 1 :]
  if args.limit is not None:
    states = states[: args.limit]
  report_path = args.output_dir / "results.csv"
  fields = (
      "sequence",
      "case",
      "backend",
      "projection",
      "parameters",
      "control_points",
      "frame_count",
      "stitch_frame_time",
      "max_output_width",
      "configured_live_dimension_cap",
      "auto_fov",
      "horizontal_fov",
      "auto_canvas",
      "auto_crop",
      "actual_backend",
      "actual_projection",
      "actual_parameters",
      "actual_max_output_width",
      "actual_live_dimension_cap",
      "actual_live_dimension_cap_applied",
      "actual_projection_auto_fov",
      "actual_projection_horizontal_fov",
      "actual_projection_auto_canvas",
      "actual_projection_auto_crop",
      "canvas_width",
      "canvas_height",
      "pto_f",
      "pto_v",
      "pto_w",
      "pto_h",
      "pto_S",
      "actual_fov",
      "actual_fov_source",
      "calibration_complete",
      "playback_restart_complete",
      "app_run_success",
      "timed_out",
      "resource_watchdog_triggered",
      "resource_failure",
      "outcome",
      "duration_seconds",
      "return_code",
      "first_failure",
      "log",
      "before_sample",
      "after_sample",
      "sample_failure",
      "work_directory",
  )
  failures = 0
  with report_path.open("w", encoding="utf-8", newline="") as report:
    writer = csv.DictWriter(report, fieldnames=fields)
    writer.writeheader()
    report.flush()
    os.fsync(report.fileno())
    final_offset = args.start_at + len(states) - 1
    for offset, state in enumerate(states, start=args.start_at):
      backend = str(state["backend"])
      projection = str(state["projection"])
      parameters = tuple(state["parameters"])
      framing = projection_framing(state)
      case = str(state.get("case", state_id(backend, projection, parameters, framing)))
      log_path = args.output_dir / f"{offset:03d}-{case}.log"
      wait_for_resource_headroom(args)
      print(f"START {offset:03d}/{args.start_at + len(states) - 1:03d} {case}", flush=True)
      work_root, game_id = create_isolated_game(args.source_game_dir, source_config_snapshot)
      result: dict[str, object] | None = None
      try:
        result = run_state(args, work_root, game_id, state, offset, log_path)
        if args.capture_samples and result["outcome"] == "pass":
          before_sample, after_sample, sample_failure = capture_case_samples(
              work_root / game_id, args.output_dir, offset, case, args.sample_width
          )
          result["before_sample"] = before_sample
          result["after_sample"] = after_sample
          result["sample_failure"] = sample_failure
          if sample_failure:
            result["outcome"] = "fail"
            if not result["first_failure"]:
              result["first_failure"] = sample_failure
      finally:
        keep_work = result is not None and result["outcome"] != "pass" and args.keep_failed_work
        if not keep_work:
          remove_work_root(work_root)
      assert result is not None
      result["work_directory"] = str(work_root) if keep_work else ""
      writer.writerow(result)
      report.flush()
      os.fsync(report.fileno())
      failures += result["outcome"] != "pass"
      print(
          f"{str(result['outcome']).upper()} {offset:03d} {case} {result['duration_seconds']}s"
          + (f" :: {result['first_failure']}" if result["first_failure"] else ""),
          flush=True,
      )
      if offset < final_offset:
        wait_for_resource_headroom(args)
  print(f"RESULTS {report_path} failures={failures} states={len(states)}", flush=True)
  return 1 if failures else 0


if __name__ == "__main__":
  raise SystemExit(main())
