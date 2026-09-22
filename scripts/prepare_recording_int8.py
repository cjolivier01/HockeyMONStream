#!/usr/bin/env python3
"""Prepare INT8 from bounded native stitched samples of a calibrated recording.

Source-checkout preparation tool; requires the optional ONNX/ORT/OpenCV Python
environment. Playback itself remains native. Never edits the game's config.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile

try:
  from benchmark_model_precision import configured_inference_path
  from quantize_detector_onnx import quantize
except ModuleNotFoundError:
  from scripts.benchmark_model_precision import configured_inference_path
  from scripts.quantize_detector_onnx import quantize


def digest(path):
  result = hashlib.sha256()
  with path.open("rb") as source:
    for chunk in iter(lambda: source.read(1024 * 1024), b""):
      result.update(chunk)
  return result.hexdigest()


def scalar(config, key):
  match = re.search(rf"^  {re.escape(key)}: (.+)$", config.read_text(), re.MULTILINE)
  if not match:
    raise ValueError(f"Detector config is missing {key}")
  return match.group(1).strip()


def sampling_command(args, samples, width, height):
  return [str(args.cli), "-c", str(args.app_config), f"--game-id={args.game_id}",
          "--enable-sources=URI-MULTIPLE", "--enable-sinks=FAKE",
          f"--int8-sample-output={samples}", f"--int8-sample-count={args.samples}",
          f"--int8-sample-width={width}", f"--int8-sample-height={height}"]


def run(command, capture=False):
  # A separate process group bounds cancellation to this preparation child.
  environment = dict(os.environ)
  environment["HSTREAM_UI_PARENT_PID"] = str(os.getpid())
  for key in ("HSTREAM_CALIBRATION_PENDING", "HSTREAM_CALIBRATION_START_STAGE",
              "HSTREAM_CALIBRATION_INVALIDATION_ID", "HSTREAM_RINK_LEVELING_FLOW", "HSTREAM_PROJECTION_CROP_FLOW"):
    environment.pop(key, None)
  process = subprocess.Popen(command, env=environment, start_new_session=True,
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE if capture else None, text=True)
  completed = False
  try:
    output, _ = process.communicate()
    completed = True
    if process.returncode:
      raise RuntimeError(f"Preparation step failed ({process.returncode}): {command[0]}")
    return output
  finally:
    if not completed:
      try:
        os.killpg(process.pid, signal.SIGTERM)
      except ProcessLookupError:
        pass
      try:
        process.wait(timeout=3)
      except subprocess.TimeoutExpired:
        pass
      # A child may exit before a grandchild that ignored TERM. Always clean
      # the entire preparation group, even when the immediate child exited.
      try:
        os.killpg(process.pid, signal.SIGKILL)
      except ProcessLookupError:
        pass
      process.wait()


def cancel(signum, frame):
  # Repeated Cancel/Escape or shutdown signals must not interrupt the
  # process-group cleanup in run()'s finally block.
  signal.signal(signal.SIGTERM, signal.SIG_IGN)
  signal.signal(signal.SIGINT, signal.SIG_IGN)
  raise KeyboardInterrupt("INT8 preparation cancelled")


def validate_samples(directory, count, width, height):
  manifest = json.loads((directory / "samples.json").read_text())
  samples = manifest["samples"]
  if (int(manifest["schema"]) != 1 or len(samples) != count or
      int(manifest["width"]) != width or int(manifest["height"]) != height):
    raise ValueError("Native sample manifest does not match the requested detector dimensions/count")
  previous = -1
  hashes = []
  for sample in samples:
    image = Path(sample["image"])
    if image.name != str(image) or image.suffix != ".png":
      raise ValueError("Invalid calibration image path")
    timestamp = int(sample["timeline_ns"])
    if timestamp <= previous:
      raise ValueError("Calibration frames must have distinct increasing timestamps")
    previous = timestamp
    hashes.append(digest(directory / image))
  return manifest, hashes


def prepare(args):
  import onnx
  import onnxruntime
  import cv2
  import numpy
  del onnxruntime, cv2, numpy
  runtime = run([str(args.builder), "--runtime-info"], capture=True).strip()
  if not runtime.startswith("HSTREAM_TENSORRT_RUNTIME "):
    raise ValueError("Builder did not report a compatible TensorRT SDK/runtime and GPU")
  gpu_match = re.search(r" gpu_name=([A-Za-z0-9_]+)$", runtime)
  if not gpu_match:
    raise ValueError("Builder did not report a safe GPU model name")
  playback_runtime = run([str(args.cli), "--tensorrt-runtime-info"], capture=True).strip()
  if runtime != playback_runtime:
    raise ValueError(f"Builder and DeepStream must use the same TensorRT version and GPU: {runtime!r} != {playback_runtime!r}")
  engine_name = f"detector_{gpu_match.group(1)}_int8.engine"
  run([str(args.assets), str(args.detector_config)])
  source = configured_inference_path(args.detector_config, "onnx-file")
  original = onnx.load(source, load_external_data=False)
  if any(t.data_location == onnx.TensorProto.EXTERNAL for t in original.graph.initializer):
    raise ValueError("Automatic preparation currently requires the bundled self-contained detector ONNX")
  dims = original.graph.input[0].type.tensor_type.shape.dim
  if len(dims) != 4 or dims[1].dim_value != 3 or dims[2].dim_value <= 0 or dims[3].dim_value <= 0:
    raise ValueError("Expected detector input NCHW with fixed spatial dimensions")
  height, width = dims[2].dim_value, dims[3].dim_value
  if any(scalar(args.detector_config, key) != value for key, value in
         (("model-color-format", "0"), ("maintain-aspect-ratio", "1"), ("symmetric-padding", "1"))):
    raise ValueError("Automatic calibration requires RGB and symmetric aspect-preserving preprocessing")
  source_hash = digest(source)
  config_hash = digest(args.detector_config)
  args.cache.mkdir(parents=True, exist_ok=True)
  with tempfile.TemporaryDirectory(prefix=".preparing-", dir=args.cache) as temporary:
    staging = Path(temporary)
    samples = staging / "samples"
    print("HSTREAM_INT8_PREPARATION stage=sampling", flush=True)
    run(sampling_command(args, samples, width, height))
    manifest, hashes = validate_samples(samples, args.samples, width, height)
    if int(manifest["gpu_id"]) != 0:
      raise ValueError("Automatic INT8 preparation currently requires the recording pipeline on CUDA GPU 0")
    recipe = {"schema": 1, "source_sha256": source_hash, "config_sha256": config_hash,
              "runtime": runtime, "capture": manifest, "image_sha256": hashes}
    identity = hashlib.sha256(json.dumps(recipe, sort_keys=True).encode()).hexdigest()
    destination = args.cache / identity
    print("HSTREAM_INT8_PREPARATION stage=quantizing", flush=True)
    qdq = staging / "detector-int8.onnx"
    quantize(argparse.Namespace(onnx=source, output=qdq, image_list=samples / "images.txt",
                                scale=float(scalar(args.detector_config, "net-scale-factor")), preprocessed=True))
    print("HSTREAM_INT8_PREPARATION stage=building", flush=True)
    engine = staging / engine_name
    run([str(args.builder), "--precision=int8", "--explicit-precision", f"--onnx={qdq}",
         f"--engine={engine}", "--batch-size=2", "--min-batch-size=1"])
    run([str(args.cli), "--int8-sample-verify", str(samples)])
    if digest(source) != source_hash or digest(args.detector_config) != config_hash:
      raise ValueError("Detector inputs changed during preparation; previous engine remains selected")
    if not engine.is_file() or engine.stat().st_size == 0 or not Path(str(engine) + ".layers.json").is_file():
      raise ValueError("Builder did not publish an inspected engine")
    recipe["engine_sha256"] = digest(engine)
    (staging / "manifest.json").write_text(json.dumps(recipe, indent=2) + "\n")
    # Every result has its own immutable bundle. Concurrent preparation never
    # replaces another engine, and cancellation never changes saved UI settings.
    if destination.exists():
      destination = args.cache / (identity + "-" + staging.name.removeprefix(".preparing-"))
    os.rename(staging, destination)
  result = {"engine": str(destination / engine_name), "manifest": str(destination / "manifest.json"),
            "samples": str(destination / "samples"), "count": args.samples}
  print("HSTREAM_INT8_READY " + json.dumps(result), flush=True)
  return result


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  for name in ("cli", "builder", "assets", "app-config", "detector-config"):
    parser.add_argument("--" + name, type=Path, required=True)
  parser.add_argument("--game-id", required=True)
  parser.add_argument("--samples", type=int, default=64)
  cache = Path(os.environ.get("HSTREAM_TENSORRT_CACHE_DIR",
               str(Path(os.environ.get("XDG_CACHE_HOME", str(Path.home() / ".cache"))) / "hstream/tensorrt")))
  parser.add_argument("--cache", type=Path, default=cache / "recording-int8")
  args = parser.parse_args()
  if not 16 <= args.samples <= 256:
    parser.error("--samples must be 16..256")
  for name in ("cli", "builder", "assets", "app_config", "detector_config", "cache"):
    setattr(args, name, getattr(args, name).resolve())
  signal.signal(signal.SIGTERM, cancel)
  signal.signal(signal.SIGINT, cancel)
  try:
    prepare(args)
  except (Exception, KeyboardInterrupt) as error:
    parser.exit(2, f"INT8 preparation failed: {error}\n")


if __name__ == "__main__":
  main()
