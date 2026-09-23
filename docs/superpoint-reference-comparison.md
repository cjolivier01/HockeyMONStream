# SuperPoint / LightGlue reference comparison

HStream's **1K (1024 px long edge)** image size reproduces the input scale used by
`hm-cupano/scripts/create_control_points.py`: the Python script's
`SuperPoint.extract()` implicitly resizes to that long edge. The detector budget
is now 2048 per camera. **Max control points** remains the subsequent match cap.
Native and 2K intentionally supply different image scales and need not find the
same correspondences as the reference.

The comparison uses identical saved camera PNGs from `tv-14-1-p1` experiment 15,
not separately decoded timestamps. The direct comparison loads both inputs with the
reference script's 8-bit `cv2.imread` policy; normal calibration preserves retained
16-bit PNG precision, which can slightly change counts. Counts below are before the control-point cap,
pooling, and geometric validation; “lower” means the left-camera point is below
half the image height.

| Pipeline | Accepted matches | Lower-half matches |
| --- | ---: | ---: |
| Previous native HStream, 1024 detector points | 95 | 2 |
| Python script, implicit 1K, 2048 detector points | 582 | 96 |
| Corrected HStream CPU, 1K, 2048 detector points | 581 | 95 |
| Corrected HStream CUDA, 1K, 2048 detector points | 580 | 95 |

The previous native graph detected 277 lower-half keypoints in the left image,
but returned only five lower-half matches. HStream's strict match-score cutoff
of `> 0.2` removed three. The cap/spatial selector removed none. Thus neither
“SuperPoint found only two points” nor “the cap discarded the near side” describes
the failure.

The changes address three differences:

1. Detector capacity increases from 1024 to the script's 2048 points.
2. The 1K path uses floating-point grayscale, Gaussian antialias filtering,
   bilinear interpolation, and the reference aspect-ratio rounding. Resizing
   integer pixels with an area filter changes weak ice features. An independent
   Kornia pixel oracle covers interior and reflected-border samples.
3. The ONNX matcher restores source pixel centers and normalizes both coordinate
   axes by each camera's original long edge. The previous export divided X and Y
   independently by the shared padded canvas. The additional `image_sizes[2,4]`
   input carries source width/height and resized width/height for each camera, so
   unequal cameras and padding do not change their positional coordinates.

## Weights and numerical parity

All 227 FP32 weight tensors in the ONNX model match the Python SuperPoint and
LightGlue checkpoints exactly, accounting for export-time matrix transposes.
Unused intermediate stopping/pruning heads are omitted from the fixed nine-layer
ONNX matcher. The new export changes capacity, shapes, and positional
normalization; it does not train or replace weights.

Numerical execution still differs. HStream's CUDA SuperPoint convolution uses
FP16 while the Python detector uses FP32. The Python reference normally enables
FP16 FlashAttention, while ONNX LightGlue uses FP32 attention. On the full-image
comparison, 577 of 582 reference pairs agree with CPU results and 563 with CUDA
results within one source pixel in the combined four-coordinate distance. Similar
counts alone are not the parity criterion.

`scripts/compare_superpoint_matcher_onnx.py` isolates the matcher by supplying
identical detector features to both implementations and disabling reference flash
attention. On the experiment pair, it verifies **581 identical match pairs** with
maximum score error **0.000058**; an unequal landscape/square pair gives **595
identical pairs**, error **0.000052**. The old positional normalization fails this
check: only 569 reference pairs are shared. The check requires exact pair identity
and maximum score error at most 0.001.

The fixed-capacity ONNX detector and the reference's detection-threshold/variable
point count remain different for inputs with fewer than 2048 eligible features.
The measured recording pairs saturate the detector budget. Final capped selection
also intentionally differs: HStream balances occupied height bands and horizontal
cells, while the Python script samples Y-sorted indices. These stages must be held
separate when interpreting a parity result.

## Reproducing the checks

Offline qualification uses Python only; installed playback remains Python-free.
Use an environment containing PyTorch, Kornia, OpenCV, ONNX 1.20.1, and ONNX
Runtime, plus a cvg/LightGlue checkout and its checkpoints. The reference run used
conda `ubuntu` (PyTorch 2.11, Kornia 0.8.3) and ONNX Runtime 1.30.

```bash
python scripts/compare_superpoint_matcher_onnx.py \
  --model /path/to/superpoint-lightglue-k2048-d63a61e3b1667c0b.onnx \
  --lightglue-root ../hm/xmodels/LightGlue \
  --left /path/to/left.png --right /path/to/right.png
```

The ONNX matcher is evaluated on CPU to isolate float32 arithmetic; `--device`
selects the PyTorch reference device. To check native inference, set
`HM_REQUIRE_ONNX_MODEL_TESTS=1`, `HM_MATCHER_SMOKE_NAME=superpoint-lightglue`,
`HM_MATCHER_SMOKE_PROVIDER=cpu` or `cuda`, `HM_SUPERPOINT_SMOKE_RESOLUTION=1k`, and
`HM_SUPERPOINT_SMOKE_GAME_DIR` to a directory containing `left.png` and `right.png`,
then run `//src/libs/stitching:native_model_smoke_test`.

## Model artifacts

`scripts/export_superpoint_cuda_onnx.py` verifies the original pinned graph hash
before conversion and preserves every learned weight. Repeated exports are
byte-identical. Both assets are published under the `pretrained-assets-v1` release;
configuration records their content hashes and downloads them on demand.

| Asset | SHA-256 |
| --- | --- |
| CPU `superpoint-lightglue-k2048-d63a61e3b1667c0b.onnx` | `d63a61e3b1667c0bdf89bea9c5508df9dc98472944ca1dea0b46f3facfc00935` |
| CUDA `superpoint-lightglue-cuda-k2048-59460a88dac888ad.onnx` | `59460a88dac888adeccb6bd4a7870b5193a0755e3b0fdc57de249177b7f47bc4` |

```bash
python scripts/export_superpoint_cuda_onnx.py upstream.onnx cuda.onnx --cpu-output cpu.onnx
```

For a byte-identical copy of the previous CUDA asset, add
`--keypoints=1024 --normalization=legacy-per-axis`. Existing saved calibrations are
retained; choose 1K and generate a new experiment to use the corrected path.
