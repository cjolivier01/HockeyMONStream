# Optional tiled detection

Native NVIDIA and AMD Program detection can process two overlapping full-height
halves or four overlapping quadrants of one stitched frame in a single model
batch. It is off by default; enabled presets without an explicit `tiles` override
retain two halves through the shared baseline.
Enable it on the next run with:

```bash
./run.sh --game-id=caha-golden-bears \
  --options=pipeline.primary-gie.tiled-inference.enable=1 \
  --options=pipeline.primary-gie.tiled-inference.tiles=4
```

In desktop and web **Program Controls → Detection**, select **Tiled detection
(next run)** and choose **Two overlapping halves** or **Four overlapping
quadrants** under **Detection tiles (next run)**. Save Preset retains both
choices; unsaved choices apply to Play.
Both controls are locked while a run or another operation owns the pipeline.
Reset Controls restores the baseline choice. The same setting can be saved in the game's
`_hockeymon/config.yaml` or the user's `~/.config/hstream-ui/defaults.yaml`:

```yaml
pipeline:
  primary-gie:
    tiled-inference:
      enable: 1
      tiles: 4
      overlap: 0.2
      merge-iou: 0.5
```

Merge these keys into any existing maps. `enable: 0` restores the original
full-frame path. There is no live switch. Both native interfaces
use the same resolved YAML; the HockeyMON Python Program backend is outside this
option's scope. Defaults live only in the shared baseline.

`tiles` must be 2 or 4, including when disabled. Overlap is a fraction of each
tile's extent along each split axis. At `overlap: 0.2`, halves are approximately
`canvas_width / 1.8` wide and retain the full height. Quadrants are approximately
`canvas_width / 1.8` by `canvas_height / 1.8`, anchored at the four corners. The
same overlap applies horizontally and vertically. Native crop transforms use
even alignment; an odd last row or column is excluded, as with ordinary NVIDIA
full-frame preprocessing. The crops collectively cover the aligned panorama.

Tiling selects detector batch two or four before TensorRT cache lookup, including
under the automatic low-memory profile. An explicit conflicting batch size fails;
clear that override or match the tile count. NVIDIA requires native `nvinfer`,
interval zero, the original stitched surface and a detector engine supporting
the selected batch. An explicitly selected custom/prepared engine must support
that batch too. Tensor-input, asynchronous classification, instance masks and
ROI size filters are unsupported. AMD compiles one dynamic-batch ONNX model at
the selected batch; its normal FP32/FP16 and model-contract restrictions still
apply. The larger input/output tensors remain subject to its configured memory
bound.

NVIDIA uses temporary untracked object regions with its existing GPU secondary
detector crop/batch path. The detector's output probe clips boxes to their crop,
removes the temporary parents and suppresses same-class cross-tile duplicates.
AMD writes all crops into one contiguous NCHW tensor, enqueues MIGraphX once,
compacts proposals on the GPU, maps per-tile results back and uses the same merge.
Tracking, rink filtering, previews and telemetry receive one original stitched
frame with normal detection metadata. Role IDs, source timestamps and frame
identity are retained. Pixels never make a GPU-to-CPU round trip; only AMD's
existing compact detection metadata is read back.

`merge-iou` controls same-class duplicate suppression across tiles. Predictions
with different role labels are retained; this option does not resolve conflicting
person/goalie/official classifications. Per-tile model thresholds and NMS still
apply, so the merged frame can contain up to the tile count times the per-tile detection limit.

Batching avoids separate model launches but does not remove the extra inference
work or memory. Four quadrants process twice as many model inputs as two halves;
latency and total memory do not necessarily scale linearly.

Full-height halves cannot enlarge people beyond the height-limited resize
scale. At 1984×736, a 12407×4710 GSE panorama is already height-limited; two
halves do not enlarge its detections. For a 12370×3113 CAHA panorama, their
maximum scale improvement is about 1.47×. An aspect ratio of 3.4:1 or wider is
a useful evaluation starting point for two halves, not an automatic switch.

Four quadrants at 20% overlap enlarge objects approximately 1.8× in each
input dimension relative to full-frame detection, independent of the panorama's
aspect ratio apart from alignment/resize rounding. This does not remove
letterbox padding or guarantee better accuracy. The horizontal seam can cut
through a person; overlap helps, but partial and full boxes may not have enough
IoU to merge. Cross-tile suppression also preserves different role labels.
Evaluate missed players, duplicate tracks and role accuracy near both seams and
the four-way overlap, alongside throughput and memory on the intended hardware.

On an RTX 4090 with the CrowdHuman medium model in FP16 and the CAHA panorama,
100-frame detector-only runs (excluding the first ten frames) averaged 5.30 ms
full-frame and 7.92 ms tiled. Nsight recorded one TensorRT enqueue per frame in
both modes; tiled inference used batch two. These numbers measure an additional
2.62 ms of detector work, not end-to-end FPS or referee classification accuracy.

Four-quadrant qualification used a later 12228×3258 CAHA still with the same
CrowdHuman medium FP16 model. On RTX 5090, 30-frame runs excluding the first ten
averaged 4.98 ms with two halves and 9.19 ms with four quadrants. The fixture
verified one inference callback per frame with the selected batch size. On
Radeon RX 9060 XT, ten four-quadrant runs completed with stable results and
approximately 38 ms per warm inference. These still-image checks establish
batch execution and metadata integrity, not accuracy or end-to-end playback FPS.

## Validation tools

`//src/apps/apps-common:tiled_inference_test` exercises metadata cleanup, role
preservation and frame identity without a GPU. Shared geometry/config tests live
in `src/libs/detection`, and `:gpu_test` compares full-frame and crop sampling
against an independent CPU reference for RGB8/RGB10/NV12/P010/FP16 on CUDA or HIP.
`//src/apps/apps-common:primary_gie_tiling_test` exercises the production YAML
parser and primary-GIE bin setup, including batch-two selection, conflicting
batches and saved quadrant preferences.

The offline `//src/apps/apps-common:tiled_inference_fixture` uploads one still
once, builds the detector through that same production parser and bin, runs the
real NVIDIA detector repeatedly, verifies exactly one inference
callback of batch one, two or four per original frame, and prints detection counts and
latency. Supply a prepared YAML with absolute asset/parser paths:

```bash
bazel-bin/src/apps/apps-common/tiled_inference_fixture \
  /path/infer.yaml /path/panorama.tif 4 0.2 0.5 100
```

Use `2` for halves or `0` for the full-frame comparison. Each run selects the
matching engine batch; use distinct cache paths for each batch. Warm the engine before
profiling and exclude initial frames. This isolates detection and is not an
end-to-end playback benchmark. `:hip_detector_fixture` accepts the same optional
`tiled-inference` map at the root of its offline inference YAML for AMD checks.
