# Native stitching feature matchers

`stitching.control_point_matcher` accepts four native, Python-free runtime
backends:

- `superpoint-lightglue` uses the existing SuperPoint + LightGlue ONNX graph
  at each camera image's original resolution on desktop/SBSA by default.
  Jetson defaults to the 2K canvas described below. In native mode, images are
  converted to grayscale floats in `[0,1]` and padded on the right/bottom with
  zeros to a shared canvas covering both images, rounded up to multiples of 8.
  A minimum 32 × 32 canvas supports the graph's fixed top-1024 operation for tiny
  inputs. For two 3840 × 2160 cameras, the tensor is `[2,1,2160,3840]` with no
  padding. Matches in padding are discarded; retained coordinates refer directly
  to the original images. The existing graph supports dynamic spatial dimensions.
  The keypoint limit remains 1024 per image.
- `dedode-lightglue` uses DeDoDe `L-C4-v2` detection, `B-upright`
  descriptors, and the `dedodeb` LightGlue weights in a fixed-shape ONNX
  graph with a 1024 × 576 RGB canvas per camera. Because one embedded checkpoint
  has no recorded redistribution grant, HStream does not host or automatically
  download this graph. A user who has
  permission to use the checkpoint can run
  `scripts/export_dedode_lightglue_onnx.py` with the three verified checkpoints
  in the PyTorch hub cache, then set `HM_DEDODE_LIGHTGLUE_ONNX_MODEL` to the
  exported graph (or place it at the documented user-cache filename).
- `loftr` uses the Apache-2.0 EfficientLoFTR outdoor optimized ONNX graph from
  `SpatialHub/efficient-loftr-onnx` revision
  `2c4515cbfd4866663db0ca1b3e02c55163dc5a75`. The UI spells out that this is
  the EfficientLoFTR variant rather than the original Kornia LoFTR graph.
- `akaze-hamming` uses OpenCV AKAZE with binary M-LDB descriptors, Hamming
  distance, a strict 0.75 Lowe ratio in both directions, and a mutual
  cross-check. It does not require a model asset.

`stitching.control_point_resolution` accepts `auto` (default), `native`, or `2k`
for SuperPoint + LightGlue. `auto` resolves to `2k` on Jetson and `native` on
desktop/SBSA. Explicit user/game/CLI `native` and `2k` selections override this
platform default; the UI displays the effective size. `2k` restores the 2048 × 1152 grayscale canvas from the
earlier doubled-resolution implementation: each camera is resized preserving
aspect ratio and padded; matches are converted back to source coordinates.
Both execution providers support these sizes; CUDA uses the graph described below.

The UI's **Image size** selector sits beside the control-point count. It remembers
the SuperPoint choice when switching matchers. For the other backends it is
disabled and displays their actual processing size: AKAZE at a maximum dimension
of 1920 pixels, EfficientLoFTR at 1600 (aligned down to multiples of 32), and DeDoDe
at 1024 × 576. These backends retain their existing size regardless of the saved
SuperPoint preference. This setting is independent of the stitched output width.

The resolution uses the normal baseline → user → game → CLI precedence; for example,
`--options=stitching.control_point_resolution=2k`. Save Preset or starting a run with
a changed UI resolution marks calibration stale from **features**. Immutable worker
generation claims include the selection, and canvas provenance version 9 records it.
Older provenance remains readable; selecting `2k` invalidates artifacts whose
resolution was not recorded. Generated worker overrides restore the previous game
setting on the next config load, so a one-run CLI override does not become a default.

`stitching.control_point_execution_provider` selects `cuda` (default) or `cpu`
for all neural matchers. AKAZE remains CPU-only. This setting lives in YAML and
has no UI switch. It follows baseline → user → game → CLI precedence, including
restoration of temporary generated worker overrides and immutable generation
claims. For example:

```yaml
stitching:
  control_point_execution_provider: cuda
  control_point_resolution: auto
```

CUDA uses visible device 0 (`CUDA_VISIBLE_DEVICES` controls visibility). If model
loading or inference exhausts GPU memory, calibration releases the CUDA session
and retries the same inputs once on CPU. Remaining frame pairs use that CPU
session. SuperPoint uses its original float32 CPU graph; startup verifies both
CPU and CUDA assets when CUDA matching is selected. Provider-specific verified
paths use `HM_FEATURE_MATCHER_CPU_ONNX_MODEL` and
`HM_FEATURE_MATCHER_CUDA_ONNX_MODEL`; the explicit
`HM_FEATURE_MATCHER_ONNX_MODEL` override remains a shared override for both.
The calibration progress window reports the switch. Image resolution, frame
count, cancellation, and generation ownership remain unchanged. Missing models,
invalid inputs, CUDA initialization/driver errors other than memory exhaustion,
and CPU failures remain errors. There is no repeated provider retry.
CPU sessions use ONNX Runtime's intra-op thread pool with `max(1, nproc - 1)`
threads, counting logical CPUs available in the process affinity mask. The
chosen count is logged; CUDA sessions keep CPU support work at one thread.
Graph nodes remain sequential to avoid increasing peak activation memory by
running independent nodes concurrently. This uses ORT threading, not an
`OMP_NUM_THREADS` setting, and applies when the session is created.
ONNX Runtime can place shape/control operators on CPU while running
convolutions and attention on CUDA. Existing valid calibration artifacts need
not be regenerated merely to change execution placement.

The ice-rink Mask2Former model also tries CUDA first, for both initial camera
orientation and the Program ice mask on the stitched image. It uses the same
one-time CPU fallback policy and reports the affected calibration stage. Its
existing 1344 × 800 input canvas, inference scale, mask postprocessing, and
publication rules are unchanged. These are calibration snapshots; no new
steady-state video transfers are introduced.

CPU fallback applies to ONNX calibration models. The live stitching blender still
requires GPU memory. If its `cudaBlend` allocation fails after the maps are ready,
the UI identifies the live blending stage and recommends reducing Max stitched
width or closing other GPU-intensive applications. Reducing control-point counts
does not reduce those panorama buffers.

Desktop x86_64 and ARM64/SBSA use ONNX Runtime 1.30.0 with CUDA 13 and cuDNN 9.
Jetson uses NVIDIA's JetPack 6 CUDA 12.6 ONNX Runtime 1.24.0 distribution with
cuDNN 9, using the ABI-24 SDK headers from 1.24.1. The ARM wheels supply only
native libraries; Python and TensorRT execution providers are not packaged.
Bazel, the CLI/UI/exported-job launch caches, and installed packages keep the
CUDA/shared provider libraries beside the core runtime for dynamic loading.
ONNX Runtime opens these providers relative to its loaded core library, so a
core-only launch cache fails even when providers appear elsewhere in
`LD_LIBRARY_PATH`. The process environment must also exist before registering
CUDA, including when calibration loads a neural matcher as its first model.
It remains alive until process exit to avoid ONNX Runtime 1.30's environment
destructor accessing CUDA provider globals after their static destruction;
individual sessions and tensors still release normally.
Run `//src/libs/onnx:session_test` with `HM_REQUIRE_CUDA_TESTS=1` to exercise
CUDA initialization and inference before any CPU session.

The CUDA SuperPoint graph extracts the two images sequentially and uses float16
for the convolution network. NMS, descriptor normalization/sampling and LightGlue
remain float32. This avoids cuDNN's signed-32-bit tensor element limit for a batch
of two 8K images and reduces activation memory without resizing. The reproducible
converter is `scripts/export_superpoint_cuda_onnx.py` (onnx 1.20.1); it verifies the
original graph's SHA-256 before transformation. The CPU setting retains the
original float32 graph. Both keep 1024 keypoints per image and the same score
threshold. Numerical differences may change the selected keypoints and matches.

On an RTX 5090, HStream's matcher processed a pair of 7680 × 4320 frames in
0.995 seconds with 45 accepted matches; 2K took 0.294 seconds with 315 accepted
matches on the same pair. The earlier 56.16 seconds / 39 matches
measurement used CPU. Jetson Orin also completed the native 8K pair on CUDA
in 21.63 seconds (47 accepted matches); its default is 2K to reduce calibration
time and memory. These times include preprocessing, inference and match
filtering, but exclude model load, image decoding and geometric calibration.
Full resolution still requires substantial memory; larger images or smaller
GPUs can trigger the CPU fallback, which is slower and needs sufficient host
RAM. Choose `2k` explicitly to reduce matching memory and time. There is no
automatic downscaling. These transfers operate on the existing calibration
snapshots, never the steady-state video path; CPU shape/control work and the
Loop's sparse descriptors do not introduce video-frame readback.

The native model smoke test uses unequal, non-aligned 4K-sized synthetic images
for SuperPoint and checks the known source-coordinate translation. To validate
saved camera frames at their original resolution, set
`HM_SUPERPOINT_SMOKE_GAME_DIR=/path/to/game` when running
`//src/libs/stitching:native_model_smoke_test`; this reads `left.png` and
`right.png` without modifying the game's calibration. Set
`HM_REQUIRE_ONNX_MODEL_TESTS=1` to fail if model assets are unavailable. Set
`HM_SUPERPOINT_SMOKE_RESOLUTION=native` or `2k` to override the platform default. Both modes
verify synthetic translation in the original source coordinates. Set
`HM_MATCHER_SMOKE_NAME=superpoint-lightglue` to test rink segmentation and only
that matcher without requiring unrelated model assets. Set
`HM_RINK_SMOKE_IMAGE=/path/to/frame.png` to compare the selected provider's rink
mask against CPU inference on a real frame (minimum intersection-over-union 0.99). Set
`HM_REQUIRE_CPU_FALLBACK=1` when reproducing GPU memory exhaustion to require
exactly one CPU retry of the selected matcher. Set
`HM_MATCHER_SMOKE_PROVIDER=cuda` to exercise CUDA (the test defaults to CPU), and
`HM_MATCHER_SMOKE_PROFILE_DIR=/existing/directory` to record operator placement
for each matcher. A successful CUDA session alone is insufficient proof: inspect
the profiles for CUDA convolution and attention kernels.

The DeDoDe and LightGlue source projects are MIT and Apache-2.0 respectively;
Kornia and the EfficientLoFTR artifact are Apache-2.0. The DeDoDe graph also
embeds a DeDoDe-B LightGlue checkpoint published from an external host without
a model-specific redistribution grant, so HStream neither hosts nor packages
that graph. Authorized SuperPoint and EfficientLoFTR model files are downloaded
at runtime with pinned SHA-256 values and are not stored in Git. Matcher models
are on-demand assets: startup downloads only the declared graph selected by the
layered `stitching.control_point_matcher` configuration; an existing explicit
matcher override is used without attempting a stock download.

The upstream SuperPoint weights are covered by
[Magic Leap's restrictive research license](https://github.com/magicleap/SuperPointPretrainedNetwork/blob/master/LICENSE):
internal, non-commercial use only, non-transferable, and no redistribution.
HStream can download the graph into an individual user's cache when selected,
and Debian packages exclude it. The CUDA graph is a separate on-demand asset,
selected only with the CUDA provider; explicit `HM_FEATURE_MATCHER_ONNX_MODEL`
overrides remain available. Package builds stage
only the redistributable EfficientLoFTR matcher graph, together with its notice
in `third_party/native_model_licenses`. Rink and hockey YOLO remain per-user
downloads; DeDoDe must be locally exported or supplied until its model-rights
record permits redistribution. Package eligibility is fail-closed: every
packaged asset must declare `redistributable: true`.

The sibling `video-stitcher` repository's CUDA AKAZE implementation has the
desired tolerance-level CPU parity, but that implementation is distributed as
part of an AGPL-3.0 project. It is not copied or linked into this MIT project.
The OpenCV implementation here independently reproduces its interoperable
AKAZE/M-LDB/Hamming behavior. It relies on HStream's downstream control-point
selection and robust homography fitting rather than copying video-stitcher's
overlap-ROI and black-border calibration heuristics. A CUDA implementation can
replace it after a compatibly licensed kernel is available.
