# Detection precision

Program Controls → Detection selects FP32, FP16, BF16 or INT8 for the bundled
YOLOv8 detector. Changes apply on the next run; Save Preset stores them in the
game configuration and includes them in exported jobs. FP32 remains the default.
“Use saved detector configuration” cancels an unsaved precision choice and keeps
custom/inherited configurations and engines intact. Reset Controls selects FP32.
Selecting a bundled precision replaces a conflicting engine override as well as
its inference config, including an inherited override.

| Precision | Preparation |
| --- | --- |
| FP32 | Built and cached by nvinfer when needed |
| FP16 | Built and cached by nvinfer when needed; no calibration |
| BF16 | Offline engine build with a matching TensorRT SDK; no calibration |
| INT8 | Offline calibration/quantization and engine build; accuracy evaluation required |

FP16/BF16 permit mixed execution with higher precision where required. The
engine's name is not evidence of its internal precision. The offline builder
checks that detailed engine inspection contains the requested tensor type and
writes `<engine>.layers.json` for inspection. It preserves FP32 network input and
output for the current `NvDsInferParseYolo` parser.

## Select the correct SDK

`src/apps/int8-calib-builder` builds against one coherent SDK selected by
`HSTREAM_TENSORRT_SDK_ROOT` (an SDK directory containing `include/` and `lib/` or
`lib64/`; a distribution `/usr` layout is also supported). The default is `/usr`,
or the Jetson sysroot for `--config=jetson`. The builder rejects headers and
loaded libraries with different major/minor versions.

Use the SDK corresponding to **DeepStream's loaded inference runtime**. A newer
system TensorRT installation or Python package is not necessarily compatible.
For example, this development host's DeepStream 9.1 links TensorRT 10.16.1 even
though the default development files are TensorRT 11.3. Check with:

```sh
readelf -d /opt/nvidia/deepstream/deepstream/lib/libnvds_infer.so | rg nvinfer
export HSTREAM_TENSORRT_SDK_ROOT=/path/to/matching/TensorRT-SDK
bazelisk build --config=opt //src/apps/int8-calib-builder:int8-calib-builder
```

Engine preparation is currently a source-checkout tool. Installed UI/CLI runs
can consume prepared engines at the paths in the inference YAMLs. Build engines
on the target GPU/runtime; do not copy desktop engines to Jetson.

## BF16

TensorRT 9/10 supports the BF16 builder flag on Ampere and later GPUs:

```sh
MODEL="$HOME/.cache/hstream/models/hm_crowdhuman_e85_yolov8_m_1984_736_dynamic_b1-b2_1984x736.onnx"
bazel-bin/src/apps/int8-calib-builder/int8-calib-builder \
  --precision=bf16 --onnx="$MODEL" \
  --engine="${MODEL}_b2_gpu0_bf16.engine" --batch-size=2 --min-batch-size=1
```

The source wrapper also supports `./run.sh --game-id=GAME --models-bf16-build`.
Keep `HSTREAM_TENSORRT_SDK_ROOT` set when the wrapper rebuilds the tool.

TensorRT 11 removes the weakly typed precision flags. It requires an already
typed BF16 ONNX with FP32 input/output boundaries and `--explicit-precision`.
This tool does not convert a floating-point ONNX to BF16 on TensorRT 11.

## INT8 with explicit Q/DQ

Use representative **stitched** frames covering lighting, camera angles and
player sizes. Keep evaluation frames/games separate. The normal detector sees
the stitched panorama; raw camera frames or repeated images are insufficient to
establish production accuracy. A bounded offline extraction from stitched video
avoids adding video readback to the running pipeline.

Create an image list with one filename per line (relative paths resolve against
the list's directory), then prepare the quantized model:

```sh
python3 -m venv /path/to/precision-tools
/path/to/precision-tools/bin/pip install onnx onnxruntime numpy opencv-python-headless
/path/to/precision-tools/bin/python scripts/quantize_detector_onnx.py \
  --onnx="$MODEL" --image-list=/path/to/stitched-images.txt \
  --output=/path/to/detector-int8-qdq.onnx
bazel-bin/src/apps/int8-calib-builder/int8-calib-builder \
  --precision=int8 --explicit-precision --onnx=/path/to/detector-int8-qdq.onnx \
  --engine="${MODEL}_b2_gpu0_int8.engine" --batch-size=2 --min-batch-size=1
```

The quantizer uses RGB, centered black padding, bilinear resize, NCHW FP32 and
normalization matching the bundled inference config. It quantizes Conv/MatMul
weights and activations symmetrically to INT8 with per-channel weights and keeps
biases floating point because TensorRT cannot consume ORT's INT32 bias DQ nodes.
It supports dynamic or batch-one FP32 input with fixed spatial dimensions. A
custom model with different preprocessing needs a matching calibration path.
The source model and an existing destination remain intact on failed conversion.

The builder validates Q/DQ presence and FP32 I/O, builds the dynamic batch
profile, deserializes and inspects the result, and publishes the engine via a
temporary file. A legacy calibration table is not required to load this engine.
The same explicit-model path also compiles with TensorRT 11; use that SDK only
when it matches the intended deployment runtime.

TensorRT 8–10 legacy image calibration remains available through
`--precision=int8 --image-list=... --calib-table=...` or the existing
`--models-int8-calibrate` wrapper. It is not universally supported: the legacy
path failed in TensorRT's compiler on this RTX 5090/10.16.1 combination, while
explicit Q/DQ succeeded. The wrapper's default raw-camera sampling is a
convenience for experiments, not a qualified production calibration set.

## Loading and validation

The BF16/INT8 inference YAMLs declare `hstream-prebuilt-precision`.
`TensorRtModelCache` requires a nonempty engine and gives DeepStream an
engine-only runtime config, removing ONNX/calibration/custom-builder fallback
inputs. Missing or incompatible engines therefore fail rather than silently
rebuilding a different precision. Existing custom configurations without this
marker retain their behavior; legacy `_bf16.engine` names also receive this
protection. No inference precision is changed live or during calibration-only
playback.

Validate prepared engines with the intended runtime, layer inspection and
`scripts/compare_detection_accuracy.py`. Include an FP32-vs-FP32 control, report
class-aware precision/recall/F1, and inspect small-player misses and track
fragmentation. `scripts/benchmark_model_precision.py` measures throughput, but
its engine filename checks alone do not establish layer precision. Keep FP32
as the default until representative held-out results justify a change.
