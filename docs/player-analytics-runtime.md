# Native player analytics

The pose stage is opt-in. Ordinary playback omits `hmplayeranalytics` entirely;
no inference engine, stream, ROI buffer or analytics pad probe is created. Drawing
preferences do not enable inference. This stack stage publishes pose metadata;
pose/jersey/action drawing and jersey/action runtime integration follow separately.

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
