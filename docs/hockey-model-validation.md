# HARPET native validation

The optional HARPET18 pose and four-action selections use frozen checkpoints
trained in HockeyMON. [Training, metrics and dataset audit](https://github.com/cjolivier01/HockeyMON/pull/208)
and [runtime settings](player-analytics-runtime.md#hockey-pose-and-actions) describe
the intended use. Artifacts are in the existing `pretrained-assets-v1` release;
the catalog verifies SHA256 before preparing engines.

Validation used an RTX 4090, native `sm_89` CUDA code and the DeepStream-compatible
TensorRT 10.16.1 runtime. Every retained validation image/sequence was replayed
through PyTorch, ONNX and FP16 TensorRT, covering dynamic batches 1,2,8. Maximum
absolute TensorRT output errors were 0.063592 for pose and 0.006663 for action,
within the established FP16 absolute/relative tolerances of 0.08/0.03. No
high-margin argmax decision changed. Four real HARPET image/box fixtures (one per
action class) match the GPU preprocessed input exactly, and decoded FP16 joints
stay within about half an input pixel of FP32 on those fixtures.

The Python environment initially used OpenCV 5's IPP HAL, which changes border
interpolation. The offline scripts now require OpenCV 4.12.0.88 with IPP disabled
to match the native fixed-point sampler. Weights and ONNX graphs remained
byte-identical; fixed-weight evaluation was repeated after this preprocessing
correction. Final test PCKh is 89.30% (78.31% stick tip); action accuracy using
estimated poses is 76.19%, or 48/63 triplets. This is an annotated-body-box,
four-class clip benchmark, not detector-to-event performance on stitched games.

The CPU regression binaries cover manifests, paired model selection, topology
metadata and causal temporal state. CUDA tests cover 17/18-joint batch strides,
four/60-class reductions and topology-aware jersey geometry. Overlay/UI tests
cover MPII/stick drawing and paired hockey model selection. The fixture and
engine replay utilities are offline only and may read pixels/distributions back;
normal playback still transfers compact metadata only.

`player_analytics_semantic_test POSE_BUNDLE ACTION_BUNDLE POSE_FIXTURE_DIR --hockey`
also passed with the published models: two tracked players run actual pose and
three-sample action inference, preserve stick joints and batch identity, and
clear action evidence after gaps and seeks. Duplicate timestamps add no evidence.

Build commands on this host require a matching local TensorRT SDK for detector
targets and an explicit `gio` link for the existing jetson-utils network library:

```bash
HSTREAM_TENSORRT_SDK_ROOT="$matching_tensorrt_sdk" bazelisk build \
  --config=opt --cpu=k8 --@rules_cuda//cuda:archs=sm_89 \
  --linkopt=-Wl,--no-as-needed --linkopt=-lgio-2.0 \
  -- //... -//src/libs/nvdsinfer_custom_impl_Yolo:plugin_lifetime_test
```

The excluded detector test includes system TensorRT 11 headers, whose registry
API differs from the installed TensorRT 10 runtime. The full unexcluded build
was attempted and fails there. This is outside the player-analytics SDK resolver.
The existing `side_by_side_video_test` also hangs in its delayed-audio FFmpeg
fixture on this machine. These limitations are recorded rather than changing
unrelated detector/video code. Qt tests require `QT_QPA_PLATFORM=offscreen` and
`--test_env=HOME` in this environment. Of the 175 supported suite tests, 173
passed initially; `scoreboard_selector_test` and `hstream_ui_test` both passed
on retry with the original PATH (5.1s and 184.0s). Their earlier failures were in
unrelated HTTP/calibration timing paths. Thus all 175 supported tests have a
passing run, with the excluded detector/video limitations above still open.

Jetson validation was attempted: `stubby` is unreachable (`No route to host`)
and `make jetson` reports that `/opt/jetson-sysroot` is absent. No Jetson build or
runtime result is claimed. Target-local engine preparation and the existing
CUDA/EGL surface import remain the deployment path on that platform.
