# Detection precision

Program Controls → Detection selects the detector model and then FP32, FP16,
BF16 or INT8 for it. Changes apply on the next run; Save Preset stores them in
the game configuration and includes them in exported jobs. FP32 remains the default.
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

## Detection model

`hstream_ui.detector_models` in `configs/baseline.yaml` lists the models the
Detection tab offers. Each entry names a `config_prefix`: the bare prefix is the
FP32 inference config and the selected precision appends `_fp16`, `_bf16` or
`_int8`, so a model contributes four `configs/config_infer_*.yaml` files. The
first entry is the default and the one Reset Controls restores.

| Model | Network | Inference size | Prefix |
| --- | --- | --- | --- |
| Default | YOLOv8-m | 1984x736 | `config_infer_yolov8_hockey` |
| Distilled | YOLOv8-s | 1408x544 | `config_infer_yolov8s_hockey` |

The distilled model is a YOLOv8-m to YOLOv8-s knowledge distillation trained at
1408x544, exported by `scripts/export_hm_yolov8_onnx.py`. It is roughly a
quarter of the default's detector cost, at some accuracy. Because its inference
resolution differs, revalidate tracking and the oversized-player thresholds on
any game switched to it rather than assuming the default's tuning carries over.

Model selection is per game: it is stored as `pipeline.primary-gie.config-file`
in the game's `config.yaml`, the same key the precision choice uses.

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
loaded libraries with different major/minor versions. If the selected SDK is not
on the system loader path, include its library directory in `LD_LIBRARY_PATH`
when invoking the builder; the SDK selection alone configures build dependencies.

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

The recording preparation helper and wrapper build commands compare the
builder's `--runtime-info` with `hstream-cli --tensorrt-runtime-info` **before
building**. The CLI resolves `getInferLibVersion` through DeepStream's
`libnvds_infer.so` dependency handle, rather than opening the system's
unversioned `libnvinfer.so`. The full TensorRT version and CUDA GPU UUID must
match. This catches a coherent TensorRT 11 builder paired with TensorRT 10
playback, in addition to the builder's SDK/header check. The CLI also prints
the library path to stderr. Inspect both executables with `ldd` when diagnosing
loader overrides.

## Engine names and GPU selection

Generated engine names include a sanitized CUDA GPU model and precision, for
example `detector_NVIDIA_GeForce_RTX_5090_int8.engine`. CUDA device properties
honor `CUDA_VISIBLE_DEVICES`; `gpu0` alone cannot distinguish different GPUs.
FP32/FP16 cache builds tag the staged ONNX name, so DeepStream's derived engine
name also contains the GPU model. Separate cache directories prevent one GPU
from overwriting another GPU's engine or runtime configuration.

Bundled inference paths contain `{gpu}` plus the precision suffix. The native
runner resolves the token using the effective inference GPU, preserving engines
for other GPUs and precisions. UI presets retain that token for ordinary bundled
choices. Recording preparation saves an explicit, immutable engine path and
provenance for that particular build. Explicit custom paths remain explicit;
incompatible prepared engines fail instead of being rebuilt over an existing
file. Resolve a template for CUDA GPU 0 with:

```sh
bazel-bin/src/apps/hstream-cli/hstream-cli --resolve-engine-path '/path/detector_{gpu}_bf16.engine'
```

## Prepare INT8 from a recording

Complete stitching calibration, stop playback, then open **Program Controls →
Detection → Prepare INT8 from recording**. Choose 16–256 samples (default 64).
The preparer samples stitched panoramas at evenly spaced interior timestamps
across the complete synchronized recording. This currently decodes/stitches the
recording once; it does not seek directly to the selected times. Detection,
tracking, audio and production outputs are disabled for this pass.

Only selected images cross to host memory, after GPU conversion to the exact
detector input dimensions (at most four megapixels). Source ROI alignment,
default NVIDIA resize filtering, truncation and symmetric black padding match
the bundled nvinfer preprocessing. The optional CPU ONNX Runtime quantizer
consumes these prepared PNGs without a second resize. Ordinary playback does
not acquire a video readback path.

Build the native tools with the SDK selected above, and install the optional
offline Python environment:

```sh
bazelisk build --config=opt //src/apps/hstream-cli:hstream-cli \
  //src/apps/hstream-ui:hstream-ui //src/apps/hstream-assets:hstream-assets \
  //src/apps/int8-calib-builder:int8-calib-builder
python3 -m venv /path/to/precision-tools
/path/to/precision-tools/bin/pip install onnx onnxruntime numpy opencv-python-headless
HSTREAM_INT8_PYTHON=/path/to/precision-tools/bin/python bazel-bin/src/apps/hstream-ui/hstream-ui
```

`HSTREAM_INT8_PREPARER` and `HSTREAM_INT8_BUILDER` optionally override source
tool paths. Automatic preparation currently supports the bundled self-contained
FP32 NCHW detector and CUDA GPU 0; unsupported inputs fail before publication.
Installed native packages do not include Python or the quantizer; they can
play previously prepared engines.

The dialog selects and saves INT8 only after capture, quantization, engine
inspection and source/geometry verification succeed. **Play from the beginning
when ready** starts Program playback at zero using the new engine. Cancellation
stops preparation children and preserves the prior detector selection. Failed
or partial capture cannot publish an engine. This is an offline preparation
pass; it does not rebuild the inference element during live playback.

Bundles live under `${HSTREAM_TENSORRT_CACHE_DIR}/recording-int8` (or the normal
per-user TensorRT cache). Each immutable bundle retains PNGs, exact paired source
timestamps, recording file bindings, stitching generation, model/config/image
hashes, GPU/runtime identity, quantized ONNX, engine and layer inspection. Game
YAML stores the engine/manifest in `hstream_ui.detector_int8` and selects the
engine through the normal `pipeline.primary-gie` overrides; exported jobs use
that same path. Keep the selected bundle available until its preset is changed.

For unattended preparation, which prints `HSTREAM_INT8_READY` and does not edit
the game configuration:

```sh
/path/to/precision-tools/bin/python scripts/prepare_recording_int8.py \
  --cli bazel-bin/src/apps/hstream-cli/hstream-cli \
  --builder bazel-bin/src/apps/int8-calib-builder/int8-calib-builder \
  --assets bazel-bin/src/apps/hstream-assets/hstream-assets \
  --app-config configs/ds_hockey_app_config.yaml \
  --detector-config configs/config_infer_yolov8_hockey_int8.yaml \
  --game-id GAME --samples 64
```

Evaluate accuracy on separate frames/games before choosing INT8 for production.

## BF16

TensorRT 9/10 supports the BF16 builder flag on Ampere and later GPUs:

```sh
MODEL="$HOME/.cache/hstream/models/hm_crowdhuman_e85_yolov8_m_1984_736_dynamic_b1-b2_1984x736.onnx"
bazel-bin/src/apps/int8-calib-builder/int8-calib-builder \
  --precision=bf16 --onnx="$MODEL" \
  --engine="${MODEL}_b2_gpu0_{gpu}_bf16.engine" --batch-size=2 --min-batch-size=1
```

The source wrapper also supports `./run.sh --game-id=GAME --models-bf16-build`.
Preparation explicitly downloads declared source assets; playback only requires
the prepared engine and labels. Wrapper precision flags replace saved UI engine
selections; later explicit `--options` retain their usual precedence.
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
  --engine="${MODEL}_b2_gpu0_{gpu}_int8.engine" --batch-size=2 --min-batch-size=1
```

The manual image-list quantizer uses RGB, centered black padding, bilinear resize, NCHW FP32 and
normalization matching the bundled inference config. It quantizes Conv/MatMul
weights and activations symmetrically to INT8 with per-channel weights and keeps
biases floating point because TensorRT cannot consume ORT's INT32 bias DQ nodes.
It supports dynamic or batch-one FP32 input with fixed spatial dimensions. A
custom model with different preprocessing needs a matching calibration path.
The source model and an existing destination remain intact on failed conversion.
Use `--preprocessed` with the native sampler's detector-sized images to preserve
the runtime's exact GPU resize/padding instead of the manual bilinear path.

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
inputs. Source ONNX assets are on-demand; runtime acquires detector assets only
after resolving game/user/CLI configuration layers. Missing or incompatible
engines therefore fail rather than silently rebuilding a different precision. Existing custom configurations without this
marker retain their behavior; legacy `_bf16.engine` names also receive this
protection. No inference precision is changed live or during calibration-only
playback.

Validate prepared engines with the intended runtime, layer inspection and
`scripts/compare_detection_accuracy.py`. Include an FP32-vs-FP32 control, report
class-aware precision/recall/F1, and inspect small-player misses and track
fragmentation. `scripts/benchmark_model_precision.py` measures throughput, but
its engine filename checks alone do not establish layer precision. Keep FP32
as the default until representative held-out results justify a change.
