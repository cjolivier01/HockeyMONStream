# Offline preparation of player models

Player inference consumes an immutable prepared bundle. Installed playback loads only
native engines; it does not import PyTorch, download weights, or build engines. These
commands are for a source checkout. Enabling an unprepared feature fails before playback.

The initial supported profiles are RTMPose-M COCO17 (256×192), hockey PARSeq
(32×128, two digits plus EOS, all 95 trained output classes), and STGCN++ COCO2D
(100 samples, generic NTU60 labels). An NTU60 prediction is not a hockey shot/pass/check
label. The action timeline is causal, 10 Hz over 9.9 seconds; published ten-clip benchmark
accuracy does not describe this deployment. HARPET18 has a different skeleton and is
not accepted by this initial COCO17 profile.

## Export environment

Use an existing GPU Python installation that already runs the HockeyMON source checkout
with compatible PyTorch, torchvision and compiled MMCV. The script creates a new virtual
environment, inherits that explicitly selected installation, installs pinned exporter
packages only in the new environment, and records the complete package list. It never
replaces the base CUDA/PyTorch installation. Python 3.14 with the local HockeyMON forks
was validated here; the exact inherited GPU package versions remain part of the recorded
preparation provenance and must be retained to reproduce an export.

```bash
scripts/create_player_model_export_env.sh /path/to/working/python /path/to/new/export-env
```

Normal builds and playback require no TensorRT environment override. The build
selects the versioned TensorRT libraries required by the configured DeepStream
installation and checks native header/runtime versions, including the build number.
Compatible installed development headers are used directly. On Linux x86_64 with
TensorRT 10.16.1 build 11, missing or incompatible headers trigger a SHA256-pinned
download of NVIDIA's matching development packages into Bazel's external repository.
Only their headers are used; libraries still come from the installed runtime.
This handles DeepStream 9.1 alongside `/usr` TensorRT 11 development files without
installing, uninstalling or modifying system packages or using a private SDK cache.

The managed fallback requires `dpkg-deb` and network access on the first fetch
(subsequent fetches use Bazel's download cache). Native version inspection uses
Python's standard-library `ctypes`, without importing the TensorRT Python package
or creating a CUDA context. The namespaced headers cannot fall back to `/usr`
accidentally. Ordinary commands are:

```bash
make perf
bazel-bin/src/apps/player-model-builder/player-model-builder --runtime-info
```

`HSTREAM_PLAYER_TENSORRT_SDK_ROOT` remains an optional advanced override for a
custom/offline SDK. An invalid override fails instead of selecting another SDK.
Keep a custom SDK outside sandbox-masked temporary directories. `DEEPSTREAM_ROOT`
is honored consistently with the main DeepStream repository.

The SDK root contains `include[/ARCH]/NvInfer.h` and `lib[/ARCH]/libnvinfer.so`,
`libnvonnxparser.so`. Jetson builds use the actual Jetson SDK (10.3 on the tested DS7.1
host). Cross-compiles use the synchronized Jetson sysroot and never download x86
headers or execute target libraries; target startup retains the full runtime identity
check. Other SDK releases require matching installed headers or a custom SDK.
Engines must be built and
validated on their eventual playback GPU, not copied from x86 to Jetson. Preparation
records the actual loaded TensorRT semantic/build versions, CUDA runtime version, GPU
name and compute capability. The CUDA runtime query may differ from the header version
when multiple CUDA minor versions are installed.

The managed 10.16 fallback extracts `libnvinfer-headers-dev` and
`libnvonnxparsers-dev` version `10.16.1.11-1+cuda13.2` from NVIDIA's Ubuntu 24.04
repository. The source URLs and SHA256 digests are pinned in
`bazel/player_tensorrt_sdk_repository.bzl`. Do not use Python TensorRT 11 to build
engines for a DeepStream TensorRT 10 runtime.

## Validation input

Supply a `.npz` containing `inputs`, float32 normalized tensors with these shapes:

| Feature | Input shape | Meaning |
|---|---|---|
| pose | `[N,3,256,192]` | RGB affine player crops with the recorded ImageNet normalization |
| jersey | `[N,3,32,128]` | RGB uint8 PIL-compatible bicubic crops, normalized to[-1,1] |
| action | `[N,2,100,17,3]` | normalized canvas x/y/confidence; second person all zero |

Use representative crop/pose sequences, including obscured or blank cases, single/two
digits, differing player sizes and action-history confidence. There is no implicit
random-input fallback. Include an accurate `--validation-description`; synthetic data
can demonstrate export feasibility but cannot establish hockey accuracy. Preparation
cycles supplied examples to exercise minimum, optimum and maximum batch1/2/8. This
checks model numerical conversion; GPU crop and metadata transforms need separate
pipeline parity tests.

## Export and build

```bash
/path/to/export-env/bin/python scripts/prepare_player_models.py export \
  --feature pose --hm-root /path/to/HockeyMON \
  --validation-inputs /path/to/pose-validation.npz \
  --validation-description 'Held-out player crops: game IDs and extraction recipe' \
  --output /path/to/new/pose-export

/path/to/export-env/bin/python scripts/prepare_player_models.py build \
  --export-dir /path/to/new/pose-export \
  --builder bazel-bin/src/apps/player-model-builder/player-model-builder \
  --output "$HOME/.cache/hstream/player-models/pose-prepared-generation"
```

`export` downloads only the chosen pinned public pose/action checkpoint, or accepts a
local `--checkpoint` with the same verified SHA256. It strictly loads every model weight,
exports ONNX opset17, and compares the original PyTorch reference, bounded export wrapper
and ONNX Runtime at minimum/optimum/maximum batch. A missing/mismatched weight is an error.
There is no uninitialized model fallback.

For jersey, add `--feature jersey --checkpoint /path/to/author-hockey-checkpoint.ckpt`.
The author checkpoint is distributed under CC-BY-NC-3.0, which restricts local commercial
use as well as redistribution; upstream PARSeq's Apache code license does not relicense
these weights. The utility neither downloads nor redistributes that checkpoint. Its
exact pinned legacy pickle contains optimizer state; offline conversion permits that
known digest only and playback never processes pickle. Unknown checkpoints require a
new supported profile and conversion review. The PARSeq exporter preserves original
AR decoding and one refinement iteration while bounding output to three token positions.
It keeps all 95 logits, so non-digit probability is not accidentally reassigned to digits.

For actions, select `--feature action`. The official NTU60 label map is retained verbatim.
It requires the matching COCO17 pose topology; dropping HARPET stick joints does not
produce a compatible skeleton.

Copy the *export* directory to Jetson and run only `build` there with the Jetson native
builder and a Python containing NumPy. The build subcommand does not import Torch,
OpenMMLab, ONNX Runtime or Python TensorRT. It checks the ONNX/reference checksums, builds
the engine, runs the native enqueueV3 path at all three batch bounds and every remaining
supplied example in bounded chunks (including a partial final batch), and compares outputs
against recorded references. FP16 uses recorded numerical tolerances and also rejects a
changed high-margin classification decision. `--precision fp32` selects the stricter
reference profile when needed. Failed validation never publishes a prepared bundle.

A prepared directory contains `manifest.json`, `model.onnx`, `model.engine`, and
`preparation.json`. The manifest is the strict runtime contract; the separate preparation
report records validation inputs/provenance/tolerances/errors and binds the manifest
checksum. Publication atomically creates a new immutable directory and refuses existing
generations. Configure the feature with its prepared bundle directory. Disabling all features
requires neither a bundle nor exporter packages.

## Actual validation and limits

The initial research pass downloaded the real supported checkpoints and exported all
three models, strictly matching weights. ONNX parity and target-local TensorRT10.16 FP16
build/inference passed on RTX5090; the integrated preparation commands additionally
exercise batch1/2/8. Tests used deterministic nonzero synthetic tensors and establish
conversion feasibility, not held-out hockey accuracy. PARSeq uses explicit `prediction[:, 0, :]` indexing and scoped explicit encoder
attention for TensorRT 10.3 compatibility; see [the conversion findings](parseq-tensorrt-compatibility.md). RTMPose FP16
reports clipping of two large constants; recorded output parity remains required.

Run `python scripts/prepare_player_models_test.py` for atomic-publication/path failure
checks. Actual model exports, native parity, coherent-SDK rejection and Jetson builds are
required release checks; a successful ordinary C++ build does not prove model availability.

New exports record the supplied row count and each case's input indices; native
preparation checks complete coverage and replays every case. Original minimum/
optimum/maximum batch fixture names remain stable. Older exports without row-count
provenance remain usable, but should be regenerated when their validation NPZ had
more rows than the maximum batch. Existing immutable bundles are unchanged.
