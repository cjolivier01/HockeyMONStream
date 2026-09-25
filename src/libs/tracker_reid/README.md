# Optional native tracker ReID

The next-run tracker settings are pipeline.tracker.reid-enable (default false)
and pipeline.tracker.reid-model (`deepstream` by default). The SDK-supplied model
downloads and prepares automatically before playback; choose it in the Players tab
or set `reid-enable: true`. No environment variable or manually prepared file is needed.
A custom overlay remains available through `reid-model: custom` and
`pipeline.tracker.reid-config-file`; a legacy config path with no model selection
also selects Custom. The overlay is prepared only when the
final playback graph creates an enabled tracker. With either tracking or this
extension off, no new config, engine, cache, or filesystem access occurs. Existing
user-supplied native tracker settings remain unchanged, including independently
enabled ReID.

The extension supports NVIDIA NvDCF (visualTrackerType 1 or 2), one low-level
configuration, and no tracker sub-batches. It preserves unrelated NvDCF tuning,
enables native appearance reassociation, and disables embedding export. This
does not establish that two different tracker IDs represent the same player.

## DeepStream default

The default follows the installed SDK's `config_tracker_NvDCF_accuracy.yml` ReID
profile and its `resnet50_market1501.etlt` model (NGC `deployable_v1.0`). It preserves
NVIDIA's preprocessing, including `keepAspc=1`. Existing matching SDK model files
are reused; missing files download into the per-user cache with SHA256
`0e5b7f702ce7e3734e45f27b819866f15f6b083048481d1afae2089136e20201`.
The native tracker prepares the engine in an isolated child before playback;
no files under the SDK installation are modified. Engine batch, workspace, and
gallery history are capped at 32, 256 MiB, and 32 respectively. The current native
tracker library digest also participates in this cache identity. Unrelated tuning
from the selected low-level NvDCF configuration is preserved.

The explicitly named `reidentificationnet-deployable-v1.2` alternative uses NVIDIA's
portable ONNX, which has different `keepAspc=0` preprocessing. Selecting one does
not silently substitute the other. The model notices are in
[`configs/player-models`](../../../configs/player-models/README.md).

## Advanced: prepare a custom reference model

The tested reference is NVIDIA ReIdentificationNet deployable_v1.2,
resnet50_market1501_aicity156.onnx. Obtain it from NVIDIA and review the linked
model terms; the engine and weights are not included in this repository.
The model is a general person ReID model, not a measured hockey-specific model.

Use a private cache directory separated by playback GPU and TensorRT SDK version.
Download the ONNX with:

    curl --fail --location --output reid.onnx 'https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/files?redirect=true&path=resnet50_market1501_aicity156.onnx'
    echo '0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b  reid.onnx' | sha256sum --check

Build on the playback GPU using trtexec from the TensorRT SDK linked by that
DeepStream installation, not whichever TensorRT Python package or development
headers happen to be installed. Inspect the SDK's library dependencies first;
TensorRT engines are not portable between the tested x86 10.16 and Jetson 10.3
runtimes. If a matching trtexec is absent, install/extract that SDK's trtexec
offline before preparation. Do not replace system libraries to make a build work.

For the tested x86 DeepStream/TensorRT 10.16.1 host, the configured NVIDIA
Ubuntu 24.04 package repository supplies a matching binary without changing
the system installation:

    apt-get download libnvinfer-bin=10.16.1.11-1+cuda13.2
    dpkg-deb -x libnvinfer-bin_10.16.1.11-1+cuda13.2_amd64.deb ./trtexec-sdk
    ./trtexec-sdk/usr/src/tensorrt/bin/trtexec --help

The banner must report TensorRT v101601. This binary requires the matching
installed TensorRT 10 shared libraries. Other SDK/GPU combinations need their
own version; this x86 binary cannot prepare the Jetson engine.

    /path/to/matching-tensorrt/bin/trtexec --onnx=reid.onnx --saveEngine=reid.onnx.engine --fp16 --minShapes=input:1x3x256x128 --optShapes=input:2x3x256x128 --maxShapes=input:8x3x256x128 --memPoolSize=workspace:256 --skipInference

Retain the source checksum, engine checksum, GPU model and compute capability,
TensorRT version, and this exact profile with the engine. Build in a staging
directory, verify native loading with the test below, and rename the completed
bundle into its private final cache location before configuring playback. Automatic model preparation uses the native helper before playback. There
is no engine builder in the steady-state tracker. A missing/unloadable engine fails the run;
the generated native config contains no ONNX/ETLT/calibration rebuild inputs.

Copy configs/player-analytics/reid-example.yaml alongside the engine. Its
modelEngineFile is relative to the overlay, not the generated runtime file.
The official ONNX contract is RGB NCHW [B,3,256,128], 256 output features,
offsets [123.675,116.28,103.53], scale 0.01735207357279195, stretched resize
(keepAspc=0), and L2 output normalization. The older ETLT sample's keepAspc=1
does not describe this ONNX. batchSize must fit the prepared engine profile.

Set reid-enable: true and reid-config-file to this overlay's absolute path in
the effective pipeline.tracker configuration. A relative reid-config-file
resolves against the structural pipeline configuration directory, as other
native tracker paths do.

## Bounds and lifetime

The overlay accepts only ReID and a limited TrajectoryManagement section.
It must specify the complete model input contract instead of inheriting a
different base model's normalization. It rejects unsupported keys, nonfinite
numbers, rebuild paths, and explicit embedding export.

Batch size is 1–64, feature size 1–4096, gallery history 1–128, extraction
interval 1–300 frames, and image sides 16–1024. The input batch and aggregate
per-stream gallery are each limited to 64 MiB; the base target count must be
1–1024. These are admission limits, not complete GPU-memory estimates.
Native NvDCF's other allocations and engine execution memory are additional.

The private mode-0600 generated YAML resides in a mode-0700 temporary directory.
Its owner is attached to the tracker GObject. It survives NULL transitions and
is deleted at final tracker destruction, including failed bin construction.
Only the generated files are removed; the source configuration and engine remain
untouched. Abrupt process termination may leave a small private temporary config.

## Verification

CPU validation:

    bazelisk run --config=opt --cpu=k8 //src/libs/tracker_reid:tracker_reid_test

Actual native loading and GPU inference, using the prepared real model:

    bazelisk build --config=opt --cpu=k8 //src/libs/tracker_reid:tracker_reid_native_test
    GST_PLUGIN_PATH=/opt/nvidia/deepstream/deepstream/lib/gst-plugins LD_LIBRARY_PATH=/opt/nvidia/deepstream/deepstream/lib bazel-bin/src/libs/tracker_reid/tracker_reid_native_test /opt/nvidia/deepstream/deepstream/samples/configs/deepstream-app/config_tracker_NvDCF_perf.yml /absolute/cache/path/reid-example.yaml

On Jetson, build with --config=jetson and use a native Jetson engine.
The test sends 48 synthetic GPU frames with explicit person detections through
HStream's lossless mux and native tracker, checks actual tracked output, and
checks runtime-config cleanup after tracker destruction. It requires the native
tracker's successful engine-load log. This is a runtime compatibility test,
not an accuracy/reassociation benchmark on real hockey players.
