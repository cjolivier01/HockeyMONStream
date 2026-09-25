# Native player analytics

Pose, jersey and action inference are opt-in. Ordinary playback omits `hmplayeranalytics` entirely;
no inference engine, stream, ROI buffer or analytics pad probe is created. Drawing
preferences do not enable inference. Immutable semantic metadata feeds optional GPU drawing.

Prepare a compatible RTMPose-M COCO17 bundle using
[the offline preparation tool](player-model-preparation.md), then add an overlay:

```yaml
pipeline:
  player-analytics:
    pose:
      enable: 1
      bundle: /absolute/path/to/prepared-pose-bundle
      rate-hz: 10
      confidence-threshold: 0.3
```

Launch with the ordinary hockey config and this overlay. Relative bundle paths
resolve against the structural app configuration directory. Settings apply on
the next run; model replacement during playback is rejected. The native tracker
must be enabled. The analytics GPU defaults to the application's GPU; an explicit
`pipeline.player-analytics.gpu-id` must identify the incoming surface's device.
Prepared engines must match the manifest, loaded TensorRT/CUDA runtime, GPU and
fixed tensor contract. There is no runtime download, build or CPU fallback.

The synchronous metadata element sits after `nvtracker`, before `vpplaytracker`.
It consumes tracked person boxes, samples padded ROIs on GPU, runs TensorRT on
one owned stream and reduces SimCC outputs on GPU. Only compact joint results
return to the CPU (at most 1,632 bytes per eight-player batch). The video surface
and all crop pixels remain on GPU. The GStreamer map accesses only the NVMM
surface descriptor. On Jetson, surface arrays use a read-only CUDA/EGL import;
logical image dimensions remain separate from aligned EGL storage dimensions;
the stream is fenced before borrowed buffers/imports are released, including
error and flush paths.

`max-tracks` (default/hard maximum 256), `max-due-rois` (32), and `batch-size`
(8) bound state and inference. Poses are attached only to the frame that generated
them. Scheduling uses stitched media PTS and full 64-bit native track IDs.
Continuous chapter transitions retain state; seeks, discontinuities, invalid
or backward timestamps, source replacement and geometry changes retire it.
Flush cancellation prevents old results from publishing. Bounded shutdown counters
report frames, enqueues, results, capacity/cadence exclusions and cancellation.

Calibration, rink-mask preparation, player-selection scans, INT8 sampling and
legacy negative stages suppress analytics before graph construction. The shared
HockeyMON baseline is unchanged. Prepared bundles and weights stay outside Git.

The plugin supports pitched NVMM RGBA8 and packed RGB10A2. This does not establish
10-bit support for unrelated upstream cropper/stitcher paths. Model inference has
an explicit GPU cost; disabled-path and enabled/drawing measurements are tracked
in [the validation record](player-analytics-validation.md).
The [benchmark procedure](player-analytics-benchmark.md) repeats frozen-runtime
comparisons and separately checks compact transfers with Nsight Systems.

## Jersey and action inference

See [the combined example](../configs/player-analytics/semantics-example.yaml).
Each feature requires its own prepared bundle and explicit `enable: 1`.
Bbox jersey mode works without pose. Pose-guided jersey requires pose enabled and
fresh same-frame shoulders/hips; it skips incomplete poses without falling back
to bbox crops. Action requires explicit COCO17 pose at at least 10 Hz. Drawing
preferences cannot satisfy either dependency.

Jersey inference uses the hockey PARSeq profile with the full 95-token vocabulary,
one/two digits followed by EOS, and preserved leading zeroes. GPU preprocessing
matches PIL bicubic antialiasing and uint8 rounding after each resize pass. Bbox
mode crops x20–80%/y25–95% of the tracked box; pose mode uses shoulders and hips
at confidence >=0.4 with 5% padding. Crops must be at least 16×8 surface pixels
and at least half visible. Source-time votes provide consensus and hysteresis;
a retained label can appear between inference updates, then expires without
repeat reinforcement. Expired/evicted identities and resets clear evidence.
The batch-eight resize workspace is about 26 MiB, allocated only with jersey enabled.

STGCN++ consumes 100 causal COCO17 samples at 100 ms intervals (9.9 seconds),
with a zero second-person slot. At least eight joints at confidence >=0.3 are
required per observation. Interpolation uses only observations already received;
gaps over 150 ms, missing identities, inadequate poses and source/seek changes
clear history and labels. Only a new complete window may run inference; repeatedly
processing one window cannot strengthen its label. The bundled model profile is
NTU60 daily activities, **not a trained hockey event detector**. Useful hockey
accuracy requires separate evaluation or training.

`max-due-rois` is an aggregate model-sample budget shared by pose, jersey and action.
Weighted rotating turns favor pose 2:1:1 and lend unused work. Guided OCR costs
two samples unless a pose is already scheduled. All admitted candidates are
considered before the cap, so early tracks with incomplete action history cannot
hide later ready tracks. Capacity and low video cadence may prevent action history
from becoming ready; the pipeline reports unknown rather than inventing poses.
The scheduler bounds analytics work without dropping video frames.

Per sample, compact D2H is 204 bytes for pose or 8 bytes for jersey/action. Action
inputs upload CPU skeleton coordinates/confidences, never video or crop pixels.
Shutdown counters include actual model enqueues/results, aggregate model samples,
maximum-frame-samples, budget-deferred, jersey pose/visibility skips and action
history readiness/resets. Interpret retained labels separately from fresh inference.


## Drawing and desktop controls

The Program Controls **Players** tab offers next-run pose, jersey and action
compute toggles, prepared bundle paths, jersey ROI mode and optional tracker ReID.
Save Preset changes only edited leaves. Unsaved choices are included in launch
and exported jobs; an active run keeps its original snapshot. Enabled-only
preflight reads bounded manifests and checks file metadata without loading engines
or touching CUDA. Playback performs the strict content/runtime checks.

Tracker ReID is independent of jersey inference. Prepare the target-local engine
and overlay using [the native ReID recipe](../src/libs/tracker_reid/README.md),
then set `pipeline.tracker.reid-enable: true` and `reid-config-file` to that
overlay. It enables NvDCF appearance reassociation; jersey numbers remain evidence
attached to native track identities and do not merge players by number.

Drawing is independent of compute:

| Canonical preference | Native override |
| --- | --- |
| `plot.plot_pose` | `pipeline.player-analytics.draw-pose` |
| `plot.plot_jersey_numbers` | `pipeline.player-analytics.draw-jerseys` |
| `plot.plot_actions` | `pipeline.player-analytics.draw-actions` |

The native key wins at the same explicit layer; a later canonical setting wins
over an older native setting. Canonical null suppresses its optional mapping.
Pose/jersey/action drawing preferences alone never enable models or create a
renderer. Player boxes inherit the boolean OR of `plot.plot_individual_player_tracking`
and `plot.debug_play_tracker`, subject to explicit native cropper overrides.
Unchanged desktop controls preserve that resolution; editing Program boxes writes
only its native drawing leaf and preserves other debug/private settings. The
preview-only Player boxes checkbox remains independent of encoded output.

Program draws after crop/rotation directly on its owned output, using the existing
CUDA stream and completion fence. Stitched preview uses the same immutable results
in the existing GL framebuffer; no additional full-frame image/copy/readback is
introduced. The existing x86/X11 preview availability is unchanged. All coordinates
use the exact per-frame crop transform, including nonuniform metadata scale and
rotation. Baked-layer bits suppress duplicate Program overlays; missing Program
transform metadata suppresses diagnostic drawing for that frame. Jersey/action
labels require the transformed player box to intersect the viewport, so fully
cropped-out players cannot leave labels clamped to its edge.

One producer assigns the 32-color palette to full 64-bit tracks before the tracked
preview tee. Boxes, skeletons and labels share those colors across views. Released
colors are reused and temporary palette overflow can recover uniqueness. Untracked
objects do not acquire a player color. Only current-frame skeletons are drawn;
jersey/action labels may persist until their evidence expires. Action text comes
from the prepared model's label map, without hockey-event renaming.

The compositor caps commands, glyphs, tile references and uploads; capacity pressure
suppresses overlays while video continues. It reuses buffers after warmup, with one
compact upload and one raster launch per nonempty visible Program frame. The font
atlas is built lazily by the same rasterizer in the CUDA/GL implementations; packages include
DejaVu Sans Mono. Empty drawing does no GPU allocation, upload or launch. Stop joins
the cropper worker, drains its stream and releases renderer resources before stream
destruction. Shutdown counters report actual launches/uploads and suppression.
