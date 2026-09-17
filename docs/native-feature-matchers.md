# Native stitching feature matchers

`stitching.control_point_matcher` accepts four native, Python-free runtime
backends:

- `superpoint-lightglue` uses the existing SuperPoint + LightGlue ONNX graph
  at each camera image's original resolution by default. Images are
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

`stitching.control_point_resolution` accepts `native` (default) or `2k` for
SuperPoint + LightGlue. `2k` restores the 2048 × 1152 grayscale canvas from the
earlier doubled-resolution implementation: each camera is resized preserving
aspect ratio and padded; matches are converted back to source coordinates.
No new ONNX export or download is needed.

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

The bundled ONNX Runtime currently uses its **CPU execution provider**, including
SuperPoint and LightGlue. Choosing native resolution does not enable CUDA.
The measured 7680 × 4320 camera pair produced 39 accepted matches in 56.16 seconds
on CPU; that was not a GPU benchmark.
Full-resolution inference increases calibration memory and runtime with source
image size; there is no automatic downscaling fallback. SuperPoint disables
ONNX Runtime's CPU memory arena so large temporary activation buffers can be
released instead of retained for reuse. Other models keep the default allocator.

The native model smoke test uses unequal, non-aligned 4K-sized synthetic images
for SuperPoint and checks the known source-coordinate translation. To validate
saved camera frames at their original resolution, set
`HM_SUPERPOINT_SMOKE_GAME_DIR=/path/to/game` when running
`//src/libs/stitching:native_model_smoke_test`; this reads `left.png` and
`right.png` without modifying the game's calibration. Set
`HM_REQUIRE_ONNX_MODEL_TESTS=1` to fail if model assets are unavailable. Set
`HM_SUPERPOINT_SMOKE_RESOLUTION=2k` to check the reduced mode instead. Both modes
verify synthetic translation in the original source coordinates.

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
but source and Debian releases do not redistribute it. Package builds stage
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
