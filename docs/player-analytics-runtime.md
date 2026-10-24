# Native player analytics

Pose, jersey, action and team inference are opt-in. Ordinary playback omits `hmplayeranalytics` entirely;
no inference engine, stream, ROI buffer or analytics pad probe is created. Drawing
preferences do not enable inference. Immutable semantic metadata feeds optional GPU drawing.

Enable the feature in Program Controls → Players, or add an overlay:

```yaml
pipeline:
  player-analytics:
    pose:
      enable: 1
      model: rtmpose-m-coco17-256x192 # optional: this is the default pose model
      rate-hz: 10
      continuous-drawing: true
      confidence-threshold: 0.3
```

Launch with the ordinary hockey config and this overlay. The selected model downloads
and prepares automatically on first use, then reuses its per-user cache. Settings apply on
the next run; model replacement during playback is rejected. The native tracker
must be enabled. The analytics GPU defaults to the application's GPU; an explicit
`pipeline.player-analytics.gpu-id` must identify the incoming surface's device.
Prepared engines must match the manifest, loaded TensorRT/CUDA runtime, GPU and
fixed tensor contract. Preparation finishes before playback; there is no inference CPU
fallback. [Preparation details](player-model-preparation.md) also document custom exports.

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
(8) bound state and inference. Fresh pose evidence is attached only to the frame
that generated it. The shared baseline defaults `pose.rate-hz` to `10`; setting
it to `fps` makes every incoming frame eligible, including fractional and variable
frame rates. The shared work budget still applies. Scheduling uses stitched media
PTS and full 64-bit native track IDs.
Continuous chapter transitions retain state; seeks, discontinuities, invalid
or backward timestamps, source replacement and geometry changes retire it.
Flush cancellation prevents old results from publishing. Bounded shutdown counters
report frames, enqueues, results, capacity/cadence exclusions and cancellation.

Calibration, rink-mask preparation, player-selection scans, INT8 sampling and
legacy negative stages suppress analytics before graph construction. The shared
HockeyMON baseline carries native cadence/drawing preferences without enabling
analytics. Prepared bundles and weights stay outside Git.

The plugin supports pitched NVMM RGBA8 and packed RGB10A2. This does not establish
10-bit support for unrelated upstream cropper/stitcher paths. Model inference has
an explicit GPU cost; disabled-path and enabled/drawing measurements are tracked
in [the validation record](player-analytics-validation.md).
The [benchmark procedure](player-analytics-benchmark.md) repeats frozen-runtime
comparisons and separately checks compact transfers with Nsight Systems.

## Jersey, action and team inference

See [the combined example](../configs/player-analytics/semantics-example.yaml).
Each feature requires explicit `enable: 1`; its supported model is selected by default.
A `model` ID makes that selection explicit. `model: custom` uses an existing `bundle`
directory instead; legacy paths without a model ID retain this custom behavior.
Bbox jersey mode works without pose. Pose-guided jersey requires pose enabled and
fresh same-frame shoulders/hips; it skips incomplete poses without falling back
to bbox crops. Action requires explicit COCO17 pose at at least 10 Hz or `fps`.
In `fps` mode actual input cadence and model budgets must still satisfy the
history's source-time gap requirements. Drawing
preferences cannot satisfy either dependency. Team needs neither pose nor a
jersey read -- it crops a chest band straight from the tracking box -- but it is
the one feature whose tuning the baseline must supply: `rate-hz`,
`warmup-samples`, `refit-interval` and `min-margin` have no compiled-in default
and an enabled `team` block without them fails startup. It has no
`confidence-threshold`; `min-margin` is the floor below which an embedding is
left unassigned.

Team affiliation is fitted per stream, not classified: two centroids over the
embeddings this game has produced, ranked dark-vs-light by mean crop lightness.
`game.home_is_dark` maps to `pipeline.player-analytics.home-is-dark` and names
the two groups; without it the overlay reads DARK and LIGHT, which is unnamed
but never wrong.

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

Frame metadata is `HSTREAM.PLAYER_ANALYTICS.V4`, schema 4, with separate fresh
COCO17 observations, retained display poses and team affiliation. All producers and consumers must
be rebuilt together; no persistent telemetry schema changes.

`max-due-rois` is an aggregate model-sample budget shared by pose, jersey, action
and team. Weighted rotating turns favor pose 2:1:1:1 and lend unused work. Team
lends most of its turns after the opening seconds, because a track leaves the due
set once its affiliation commits under a balanced, lightness-ranked fit. An
unpublishable fit supplies no track votes and keeps collecting bounded samples
until it can be replaced. Each accepted refit reopens settled tracks until a
read confirms their side or contrary evidence clears switching hysteresis. A
track occluded past `track-retention-ms` returns as a new identity and re-warms.
A published fit is retained when a refit collapses both centroids into one old
squad, even when within-squad variation follows the previous split axis. Expect a trickle, not zero, and expect the badge to drop
out while a skater is re-acquired. Guided OCR costs
two samples unless a pose is already scheduled. All admitted candidates are
considered before the cap, so early tracks with incomplete action history cannot
hide later ready tracks. Capacity and low video cadence may prevent action history
from becoming ready; the pipeline reports unknown rather than inventing poses.
The scheduler bounds analytics work without dropping video frames.

Per sample, compact D2H is 204 bytes for pose or 8 bytes for jersey/action. Team is
the exception: 4 bytes of crop lightness plus the raw 1 KiB embedding, because the
two-means fit that turns an embedding into a side is a host-side per-stream model
and no reduction kernel can stand in for it. Action
inputs upload CPU skeleton coordinates/confidences, never video or crop pixels.
Shutdown counters include actual model enqueues/results, aggregate model samples,
maximum-frame-samples, budget-deferred, jersey pose/visibility skips and action
history readiness/resets. Interpret retained labels separately from fresh inference.


## Drawing and desktop controls

`pipeline.player-analytics.pose.continuous-drawing` defaults to `true` in the
shared baseline. Program Controls → Players exposes **Keep pose visible between
updates** and **Pose updates per second / fps**. Both apply on the next run and
are included in saved presets and exported jobs.

Between inference updates, `PoseDrawingState` reprojects the latest skeleton
from its original tracked box into the current box, preserving joint confidence.
It expires after 250 ms of source time and clears when the track disappears,
its identity expires, or the source/geometry/playback state resets. Only compact
coordinates are retained; this adds no video readback or model work. Metadata
keeps this display estimate and its original observation time separate from fresh
pose evidence. Both Program CUDA drawing and GPU previews use it; action history
and pose-guided jersey crops continue to consume fresh inference only. Setting
the flag to `false` draws skeletons only on their inference frames.

Player boxes show camera-excluded players in gray with `IGNORED` above the box,
in both Program output and GPU previews. The marker comes from the current native
play-tracker result, including largest-count and oversized exclusions; it does not
remove the player from object tracking or analytics. `cam_ignore_largest` enables
`cam_ignore_largest_count` (default one), and the size filters always retain at
least three players. With three or fewer tracked players, none are size-excluded.
Disabling player boxes also hides these labels.
Jersey/action text uses a separate row for ignored players so it cannot cover
`IGNORED`; that row stays stable across the independent box visibility switch.

`PlayTrackerCtx` stores the per-frame exclusion flag in application-owned
`NvDsObjectMeta::misc_obj_info[0]`. `PreviewOverlayMeta` freezes it beside each
rectangle before the tracked tee, so Program transformations cannot change the
Stitched view's decision. The existing CUDA/GL command compositors draw the labels;
there is no video readback or separate inference pass.

The Program Controls **Players** tab offers next-run pose, jersey, action and
team compute toggles, model selectors, jersey ROI mode and optional tracker ReID.
Save Preset changes only edited leaves. Unsaved choices are included in launch
and exported jobs; an active run keeps its original snapshot. Enabled-only
preflight validates model choices and dependencies without loading engines or touching
CUDA. Custom models also receive bounded manifest/file checks. Startup downloads and
prepares only enabled supplied models; playback performs strict content/runtime checks.

Tracker ReID is independent of jersey inference. Set `pipeline.tracker.reid-enable: true`
or enable appearance matching in the Players tab. The default `reid-model: deepstream`
uses the SDK's supplied NvDCF ReID model and preprocessing. If the SDK model is missing,
HStream downloads that exact model from NVIDIA, then lets DeepStream prepare its engine
in the user cache. `reidentificationnet-deployable-v1.2` selects the newer ONNX alternative. `reid-model: custom` with `reid-config-file` retains the
[custom native ReID recipe](../src/libs/tracker_reid/README.md). It enables NvDCF appearance reassociation; jersey numbers remain evidence
attached to native track identities and do not merge players by number.

Drawing is independent of compute:

| Canonical preference | Native override |
| --- | --- |
| `plot.plot_pose` | `pipeline.player-analytics.draw-pose` |
| `plot.plot_jersey_numbers` | `pipeline.player-analytics.draw-jerseys` |
| `plot.plot_actions` | `pipeline.player-analytics.draw-actions` |
| `plot.plot_teams` | `pipeline.player-analytics.draw-teams` |

The native key wins at the same explicit layer; a later canonical setting wins
over an older native setting. Canonical null suppresses its optional mapping.
Pose/jersey/action drawing preferences alone never enable models or create a
renderer. Player boxes inherit the boolean OR of `plot.plot_individual_player_tracking`
and `plot.debug_play_tracker`, subject to explicit native cropper overrides.
Unchanged desktop controls preserve that resolution; editing Program boxes updates
its canonical and native drawing leaves and preserves other debug/private settings. The
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

The GL compositor accepts native panorama coordinates above 16K for both player
overlays and the stitched watermark. Those coordinates map into the bounded
preview viewport; they do not allocate a panorama-sized texture. Command/upload
limits and the viewport-scaled fragment budget still bound rendering work.

One producer assigns the 32-color palette to full 64-bit tracks before the tracked
preview tee. Boxes, skeletons and labels share those colors across views. Released
colors are reused and temporary palette overflow can recover uniqueness. Untracked
objects do not acquire a player color. Skeletons use fresh observations or, when
continuous drawing is enabled, tracked-box display poses retained for up to 250 ms;
jersey/action labels may persist until their evidence expires. Action text comes
from the prepared model's label map, without hockey-event renaming.

The compositor caps commands, glyphs, tile references and uploads; capacity pressure
suppresses overlays while video continues. It reuses buffers after warmup, with one
compact upload and one raster launch per nonempty visible Program frame. The font
atlas is built lazily by the same rasterizer in the CUDA/GL implementations; packages include
DejaVu Sans Mono. Empty drawing does no GPU allocation, upload or launch. Stop joins
the cropper worker, drains its stream and releases renderer resources before stream
destruction. Shutdown counters report actual launches/uploads and suppression.
