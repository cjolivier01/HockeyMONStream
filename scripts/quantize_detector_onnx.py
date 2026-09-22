#!/usr/bin/env python3
"""Prepare calibrated INT8 Q/DQ ONNX offline; never reads live pipeline buffers.

Requires onnx, onnxruntime (CPU is sufficient), numpy and opencv-python-headless.
The image list must contain representative stitched frames, one path per line.
"""

import argparse
import os
from pathlib import Path
import tempfile


def image_paths(list_path):
  paths = []
  for line in list_path.read_text().splitlines():
    if not line.strip():
      continue
    path = Path(line.strip()).expanduser()
    if not path.is_absolute():
      path = list_path.parent / path
    if not path.is_file():
      raise ValueError(f"Calibration image does not exist: {path}")
    paths.append(path)
  if not paths:
    raise ValueError("Calibration image list is empty")
  return paths


def prepare_image(image, height, width, scale, preprocessed=False):
  import cv2
  import numpy as np

  if preprocessed:
    if image.shape[:2] != (height, width):
      raise ValueError("Native calibration image must already match the network dimensions")
    rgb = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
    return np.ascontiguousarray((rgb.astype(np.float32) * scale).transpose(2, 0, 1)[None])
  ratio = min(width / image.shape[1], height / image.shape[0])
  resized_width = min(width, max(1, int(image.shape[1] * ratio + 0.5)))
  resized_height = min(height, max(1, int(image.shape[0] * ratio + 0.5)))
  image = cv2.cvtColor(image, cv2.COLOR_BGR2RGB)
  resized = cv2.resize(image, (resized_width, resized_height), interpolation=cv2.INTER_LINEAR)
  padded = np.zeros((height, width, 3), dtype=np.uint8)
  x, y = (width - resized_width) // 2, (height - resized_height) // 2
  padded[y:y + resized_height, x:x + resized_width] = resized
  return np.ascontiguousarray((padded.astype(np.float32) * scale).transpose(2, 0, 1)[None])


def quantize(args):
  import cv2
  import onnx
  from onnxruntime.quantization import CalibrationDataReader, QuantFormat, QuantType, quantize_static

  paths = image_paths(args.image_list.resolve())
  model = onnx.load(args.onnx, load_external_data=False)
  initializer_names = {value.name for value in model.graph.initializer}
  inputs = [value for value in model.graph.input if value.name not in initializer_names]
  if len(inputs) != 1:
    raise ValueError("Expected one detector image input")
  tensor = inputs[0].type.tensor_type
  dims = tensor.shape.dim
  if (tensor.elem_type != onnx.TensorProto.FLOAT or len(dims) != 4 or dims[1].dim_value != 3
      or dims[2].dim_value <= 0 or dims[3].dim_value <= 0 or dims[0].dim_value not in (0, 1)):
    raise ValueError("Expected FP32 NCHW with dynamic or single-image batch and fixed 3xHxW dimensions")
  if any(node.op_type in ("QuantizeLinear", "DequantizeLinear") for node in model.graph.node):
    raise ValueError("Input model is already quantized")
  height, width = dims[2].dim_value, dims[3].dim_value

  class Reader(CalibrationDataReader):
    def __init__(self):
      self.rewind()

    def rewind(self):
      self.index = 0

    def get_next(self):
      if self.index == len(paths):
        return None
      path = paths[self.index]
      image = cv2.imread(str(path), cv2.IMREAD_COLOR)
      if image is None:
        raise ValueError(f"Could not decode calibration image: {path}")
      self.index += 1
      print(f"Calibrating {self.index}/{len(paths)}: {path}", flush=True)
      return {inputs[0].name: prepare_image(image, height, width, args.scale, getattr(args, "preprocessed", False))}

  # Keep the previous model intact on failure or cancellation. TensorRT's
  # existing engine builder consumes this graph separately with --explicit-precision.
  args.output.parent.mkdir(parents=True, exist_ok=True)
  with tempfile.TemporaryDirectory(prefix=".int8-", dir=args.output.parent) as temporary:
    output = Path(temporary) / "detector.onnx"
    quantize_static(
      str(args.onnx), str(output), Reader(),
      quant_format=QuantFormat.QDQ,
      activation_type=QuantType.QInt8,
      weight_type=QuantType.QInt8,
      per_channel=True,
      op_types_to_quantize=["Conv", "MatMul"],
      extra_options={
        "ActivationSymmetric": True,
        "WeightSymmetric": True,
        "DedicatedQDQPair": True,
        # TensorRT accepts INT8 Q/DQ weights/activations, but not the
        # INT32 bias DQ nodes produced by ORT's default bias quantizer.
        "QuantizeBias": False,
      },
    )
    prepared = onnx.load(output)
    onnx.checker.check_model(prepared)
    if not any(node.op_type == "QuantizeLinear" for node in prepared.graph.node):
      raise ValueError("Quantizer produced no INT8 Q/DQ operations")
    os.replace(output, args.output)
  print(f"Wrote calibrated INT8 Q/DQ model: {args.output}")


def main():
  parser = argparse.ArgumentParser(description=__doc__)
  parser.add_argument("--onnx", type=Path, required=True)
  parser.add_argument("--image-list", type=Path, required=True)
  parser.add_argument("--output", type=Path, required=True)
  parser.add_argument("--preprocessed", action="store_true", help="Images already have the detector's resize and padding")
  parser.add_argument("--scale", type=float, default=0.0039215697906911373,
            help="Detector input scale (default matches bundled RGB/symmetric-padding config)")
  args = parser.parse_args()
  import math
  if not math.isfinite(args.scale) or args.scale <= 0:
    parser.error("--scale must be finite and positive")
  if args.onnx.resolve() == args.output.resolve():
    parser.error("--output must differ from the source ONNX")
  try:
    quantize(args)
  except (ImportError, ValueError, OSError) as error:
    parser.exit(2, f"quantize_detector_onnx: {error}\n")


if __name__ == "__main__":
  main()
