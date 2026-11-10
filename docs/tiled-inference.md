# Optional two-tile detection

Only the two overlapping full-height halves mode is supported. The quadrant
selector is removed. A saved `tiles: 2` or `tiles: 4` field is ignored; enabling
tiling always selects batch two. The enable, overlap and merge-IoU preferences
retain their values.

Native NVIDIA and AMD Program detection can process two overlapping horizontal
crops of one stitched frame in a single model batch. It is off by default.
Enable it on the next run with:

```bash
./run.sh --game-id=caha-golden-bears \
  --options=pipeline.primary-gie.tiled-inference.enable=1
```

In desktop and web **Program Controls → Detection**, select **Two-tile detection
(next run)**. Save Preset retains the choice; an unsaved choice applies to Play.
The checkbox is locked while a run or another operation owns the pipeline.
Reset Controls restores the baseline choice. The same setting can be saved in the game's
`_hockeymon/config.yaml` or the user's `~/.config/hstream-ui/defaults.yaml`:

```yaml
pipeline:
  primary-gie:
    tiled-inference:
      enable: 1
      overlap: 0.2
      merge-iou: 0.5
```

Merge these keys into any existing maps. `enable: 0` restores the original
full-frame path. There is no live switch. Both native interfaces
use the same resolved YAML; the HockeyMON Python Program backend is outside this
option's scope. Defaults live only in the shared baseline.

Overlap is a fraction of each tile's width, so each crop is approximately
`canvas_width / (2 - overlap)` wide. Both crops retain the full height. Native
crop transforms use even alignment; an odd last row or column is excluded, as
with ordinary NVIDIA full-frame preprocessing. No vertical rink region is removed.

Tiling selects detector batch two before TensorRT cache lookup, including under
the automatic low-memory profile. An explicit conflicting batch size fails;
clear that override or select two. NVIDIA requires native `nvinfer`, interval
zero, the original stitched surface and a detector engine that supports batch
two. Tensor-input, asynchronous classification, instance masks and ROI size
filters are unsupported. AMD compiles one dynamic-batch ONNX model at batch two;
its normal FP32/FP16 and model-contract restrictions still apply. The larger
input/output tensors remain subject to its configured memory bound.

NVIDIA uses temporary untracked object regions with its existing GPU secondary
detector crop/batch path. The detector's output probe clips boxes to their crop,
removes the temporary parents and suppresses same-class cross-tile duplicates.
AMD writes both crops into one contiguous NCHW tensor, enqueues MIGraphX once,
compacts proposals on the GPU, maps per-tile results back and uses the same merge.
Tracking, rink filtering, previews and telemetry receive one original stitched
frame with normal detection metadata. Role IDs, source timestamps and frame
identity are retained. Pixels never make a GPU-to-CPU round trip; only AMD's
existing compact detection metadata is read back.

`merge-iou` controls same-class duplicate suppression across tiles. Predictions
with different role labels are retained; this option does not resolve conflicting
person/goalie/official classifications. Per-tile model thresholds and NMS still
apply, so the merged frame can contain up to twice the per-tile detection limit.

Batching avoids a second model launch but does not remove the extra inference
work or memory. Full-height crops also cannot enlarge people beyond the
height-limited resize scale. At 1984×736, a 12407×4710 GSE panorama is already
height-limited; tiling does not enlarge its detections. For a 12370×3113 CAHA
panorama, the maximum scale improvement is about 1.47×. Accuracy and throughput
must be evaluated on the intended recording and hardware.

A practical starting point is an aspect ratio of 3.4:1 or wider, where the
geometric size gain reaches about 25%. This is a suggested evaluation threshold,
not an automatic switch or an accuracy guarantee.

On an RTX 4090 with the CrowdHuman medium model in FP16 and the CAHA panorama,
100-frame detector-only runs (excluding the first ten frames) averaged 5.30 ms
full-frame and 7.92 ms tiled. Nsight recorded one TensorRT enqueue per frame in
both modes; tiled inference used batch two. These numbers measure an additional
2.62 ms of detector work, not end-to-end FPS or referee classification accuracy.

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
callback of batch one or two per original frame, and prints detection counts and
latency. Supply a prepared YAML with absolute asset/parser paths:

```bash
bazel-bin/src/apps/apps-common/tiled_inference_fixture \
  /path/infer.yaml /path/panorama.tif 1 0.2 0.5 100
```

Use `0` instead of `1` for the full-frame comparison. Warm the engine before
profiling and exclude initial frames. This isolates detection and is not an
end-to-end playback benchmark. `:hip_detector_fixture` accepts the same optional
`tiled-inference` map at the root of its offline inference YAML for AMD checks.
