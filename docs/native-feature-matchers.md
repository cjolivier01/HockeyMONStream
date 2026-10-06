# Native stitching feature matchers

## Upgrading from a SuperPoint default

The shipped default changed from `superpoint-lightglue` to `akaze-hamming`. Two
consequences for existing games:

- **Calibration is invalidated for most games that inherited the default.** The
  next save or run reports `the selected control-point matcher changed` and
  recalibrates. Two exceptions keep their existing artifacts: a game that pins
  `stitching.control_point_matcher` explicitly, and a game whose `config.yaml`
  declares *none* of the tracked stitching settings (matcher, mapping backend,
  projection, camera config or FOV, control-point resolution, frame selection,
  manual control points) — that game skips the algorithm comparison entirely
  and keeps SuperPoint-derived artifacts until something else invalidates them.
  To keep the previous behavior deliberately, set
  `stitching.control_point_matcher: superpoint-lightglue` in the user or game
  layer. A custom `HM_CONFIG_ROOT` baseline must declare the key at all; there
  is no compiled-in fallback, so omitting it is a startup error.
- **A game holding a fisheye lens profile now fails on the shipped mapping
  backend.** Only AKAZE reads `left_calibration.json`, so under the old default
  the file was inert. With AKAZE selected, a present profile plus the shipped
  `nona` backend is rejected: *"Calibrated AKAZE control points are rectified
  and require an OpenCV mapping backend"*. Either select an OpenCV
  `stitching.mapping_backend`, pin a different matcher, or move the profile out
  of the game directory.

Saved-point replay uses recorded canvas provenance to determine whether its
points are rectified. A new default or a later lens-profile file does not turn
saved original-image points into calibrated AKAZE points. Replay still rejects
points recorded with a calibrated AKAZE profile under `nona`.

`stitching.control_point_matcher` accepts four native, Python-free runtime
backends. The default is `akaze-hamming`: it is the only backend that needs no
model asset, so a stock configuration calibrates without downloading a matcher
graph. This covers the matcher only — calibration still downloads the ice-rink
Mask2Former model and still creates that session on CUDA first, so a stock run
is not yet GPU-free or download-free end to end.

AKAZE is the default on match quality, not just on packaging: across thousands
of real stitching matches on rink footage, SuperPoint + LightGlue produced
worse alignments than AKAZE. On one saved 7680 × 4320 rink pair, both backends
on the CPU provider:

| Matcher | Accepted matches | Time |
| --- | --- | --- |
| `akaze-hamming` | 594 | 0.28 s |
| `superpoint-lightglue` at `2k` | 417 | 1.92 s |

Reproduce one row at a time, switching `HM_MATCHER_SMOKE_NAME` between
`akaze-hamming` and `superpoint-lightglue`:

```
HM_REQUIRE_ONNX_MODEL_TESTS=1 HM_SUPERPOINT_SMOKE_GAME_DIR=/path/to/game \
  HM_MATCHER_SMOKE_NAME=akaze-hamming \
  bazel test //src/libs/stitching:native_model_smoke_test \
  --test_output=all --nocache_test_results
```

The counts are printed, so `--test_output=all` is required to see them and
`--nocache_test_results` to re-run. `HM_REQUIRE_ONNX_MODEL_TESTS=1` matters
even for AKAZE: the test also loads the rink model, and without it a missing
asset makes the run skip and still report success. The synthetic fixtures
in this repo are zero-parallax self-crops and say nothing about relative
quality, so do not infer the ordering from the tests or from the fact that
SuperPoint is the learned backend. Change the default only against measured
results on real footage.

- `akaze-hamming` (default) uses OpenCV AKAZE with binary M-LDB descriptors,
  Hamming distance, a strict 0.75 Lowe ratio in both directions, and a mutual
  cross-check. It requires no model asset, runs on CPU, retains at most 2000
  detector keypoints per image, and processes at a maximum dimension of 1920
  pixels. It is the only backend that consumes the optional fisheye lens
  profile: a single `left_calibration.json` in the game directory, which
  carries both cameras as `left_uniforms` and `right_uniforms` (there is no
  `right_calibration.json`). When present, matching runs on undistorted frames and the profile
  fingerprint enters canvas provenance, so adding or removing it invalidates
  existing calibration. When absent, calibration logs `AKAZE lens calibration
  not found at ...` to stderr and matches the original camera frames — that
  notice is informational, not an error. A present profile requires an OpenCV
  mapping backend; the shipped `nona` backend rejects it, as described in the
  upgrade note above.

  AKAZE also assumes a specific two-camera overlap geometry, which the neural
  backends do not. Detection is masked to the facing half of each frame (right
  half of the left camera, left half of the right camera) and to the vertical
  band `y ∈ [0.05, 0.95]`. A match then survives only if it lies in the facing
  half of both frames, within `y ∈ [0.2, 0.8]` in both, and the two rows agree
  to within 8% of image height. That row-agreement test assumes near-rectified
  cameras, so a rig whose overlap falls outside the inner halves, or whose
  horizons differ by more than 8% of frame height, can yield no usable matches
  regardless of scene texture. Filtering needs at least 8 survivors, and 6
  after fundamental-matrix rejection. The corresponding failures read
  `AKAZE produced no usable M-LDB descriptors`,
  `AKAZE produced fewer than eight mutual overlap matches for epipolar
  filtering`, and `AKAZE fundamental-matrix filtering retained fewer than six
  matches`; all three point at rig geometry or overlap, not at the limit above.
- `superpoint-lightglue` uses the existing SuperPoint + LightGlue ONNX graph
  with the 2K canvas described below by default on every platform. In explicit native mode, images are
  converted to grayscale floats in `[0,1]` and padded on the right/bottom with
  zeros to a shared canvas covering both images, rounded up to multiples of 8.
  A minimum 48 × 48 canvas supports the graph's fixed top-2048 operation for tiny
  inputs. For two 3840 × 2160 cameras, the tensor is `[2,1,2160,3840]` with no
  padding. Matches in padding are discarded; retained coordinates refer directly
  to the original images. The existing graph supports dynamic spatial dimensions.
  The SuperPoint keypoint limit is 2048 per image, matching the HockeyMON/cupano script's detector budget.
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

**Max control points** (`stitching.max_control_points`) limits retained matched
correspondences per synchronized frame pair, not raw detections. SuperPoint still extracts at most
2048 keypoints per image; valid LightGlue matches must score strictly above 0.2.
The default AKAZE path instead retains at most 2000 detector keypoints per
image and applies no score threshold: its score is `1 - hamming/bits`, used
only to rank candidates during selection.
The UI accepts limits from 10 to 5000. General calibration and saved-point replay
require at least 10 usable matches. AKAZE drops to a specialized six-match floor
only when it is paired with an OpenCV mapping backend; with the shipped `nona`
backend the default AKAZE path still requires 10. General MAGSAC calibration still requires at least eight inliers and checks
their spatial coverage at a 10-point budget, so a small or poorly distributed set
can fail calibration. Calibrated AKAZE retains its separate small-set consensus and
coverage rules.
Selection uses a 16×9 grid in the left camera: it shares the budget across occupied
height bands, then across occupied columns within each band, ranking by confidence
within each cell. Partial rounds alternate opposite occupied edges rather than
favoring the top of the image. Sparse bands return their unused budget; a cap above
the accepted count retains every match. This preserves available near-side matches
when a broad textured wall has more populated cells, but cannot create detections
on featureless ice. Multi-frame calibration concatenates each pair's capped
selection before geometric validation, with no second global cap: 100 control
points with two frame pairs contributes up to 200. A pair with only 37 usable
matches contributes 37, without increasing another pair's allowance. If pooled
geometry fails, the existing individual-pair retries each retain their own cap.

HockeyMON's Python `hmlib/stitching/control_points.py` uses the same distinction
between detector keypoints and retained matches, also defaults to 2048 detector
keypoints, but selects evenly spaced *indices* after sorting by Y. That preserves
the original density distribution rather than allocating equal height-band budgets.
For reference-like coverage, select **1K (1024 px long edge)**. The Python script's
`SuperPoint.extract()` implicitly resizes the long edge to 1024 before inference;
`native` and `2k` are different inputs, and larger inputs do not guarantee more
useful matches. See [the reference comparison](superpoint-reference-comparison.md).

`stitching.control_point_resolution` accepts `2k` (default), `auto`, `native`, or `1k`
for SuperPoint + LightGlue. Missing settings and existing `auto` values resolve to `2k` on every platform.
Explicit user/game/CLI selections override the default; the UI displays the effective size.
The 2K default bounds feature-matching GPU memory, avoiding native 8K activation peaks that can exhaust even
a 32 GiB desktop GPU. This changes matching input size, not panorama output resolution.
`1k` independently resizes
each camera to a 1024-pixel long edge using floating-point Gaussian antialiasing
and bilinear interpolation, matching the reference Kornia preprocessing. Only
batch padding is aligned to multiples of eight; matched coordinates are restored
to source pixel centers. `2k` restores the 2048 × 1152 grayscale canvas from the
earlier doubled-resolution implementation: each camera is resized preserving
aspect ratio and padded; matches are converted back to source coordinates.
Both execution providers support all three sizes; CUDA uses the graph described below.

The UI's **Image size** selector sits beside the control-point count. It remembers
the SuperPoint choice when switching matchers. For the other backends it is
disabled and displays their actual processing size: AKAZE at a maximum dimension
of 1920 pixels, EfficientLoFTR at 1600 (aligned down to multiples of 32), and DeDoDe
at 1024 × 576. These backends retain their existing size regardless of the saved
SuperPoint preference. This setting is independent of the stitched output width. Stitching Experiments exposes the
same choices and freezes the selected size per candidate, including saved history and promotion.

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
  control_point_resolution: 2k
```

CUDA uses visible device 0 (`CUDA_VISIBLE_DEVICES` controls visibility). If model
loading or inference exhausts GPU memory, calibration releases the CUDA session
and retries the same inputs once on CPU. Remaining frame pairs use that CPU
session. SuperPoint uses the matching 2048-keypoint float32 CPU graph; startup verifies both
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
original graph's SHA-256 before transformation. It emits both a float32 CPU graph
and the serial mixed-precision CUDA graph, each with 2048 keypoints. The additional
`image_sizes` input carries each camera's source and resized dimensions. LightGlue
normalizes restored source pixel centers by that camera's long edge, rather than
scaling X and Y independently by the padded batch canvas. Old custom 1024-keypoint
ONNX overrides must be regenerated for this contract. `--keypoints=1024
--normalization=legacy-per-axis` reproduces the earlier CUDA asset for comparisons.
The score threshold is unchanged. Numerical differences may change selected
keypoints and matches; the Python reference also uses different attention precision
when its default FlashAttention path is active.

All sessions created through `src/libs/onnx/OnnxSession.cpp`, including CPU
fallbacks and rink segmentation, request deterministic computation with
`Ort::SessionOptions::SetDeterministicCompute(true)`. ONNX Runtime uses
deterministic GPU kernels where supported; this is not a guarantee of bitwise
equality across hardware, runtime versions, execution providers, or precision
modes. In particular, a CPU fallback can still change keypoints and matches.
ONNX Runtime 1.30's CUDA `TopK` implementation can also report
`Non-deterministic TopKImpl kernel is called` for this graph even with the
option enabled. Identical outputs in repeated tests therefore establish
repeatability for those inputs and that environment, not a strict guarantee
for every input on the same GPU.
The shipped SuperPoint/LightGlue graphs contain no random operators, so setting
a random seed alone does not address GPU numerical variability. Check
repeatability using identical saved camera images, model files, resolution,
and provider, comparing keypoints, match indices, scores, and selected control
points both within a session and across fresh processes.

On an RTX 5090, SuperPoint + LightGlue on CUDA processed a pair of 7680 × 4320 frames in
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
saved camera frames, set
`HM_SUPERPOINT_SMOKE_GAME_DIR=/path/to/game` when running
`//src/libs/stitching:native_model_smoke_test`; this reads `left.png` and
`right.png` without modifying the game's calibration. Set
`HM_REQUIRE_ONNX_MODEL_TESTS=1` to fail if model assets are unavailable. Set
`HM_SUPERPOINT_SMOKE_RESOLUTION=native` or `1k` to override the 2K default. All SuperPoint synthetic
modes verify translation in the original source coordinates; the real-image
mode asserts accepted-match counts instead. Despite its name,
`HM_SUPERPOINT_SMOKE_GAME_DIR` now selects real frames for whichever backend
is under test, not only SuperPoint. Set
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
