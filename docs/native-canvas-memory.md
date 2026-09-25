# Native-canvas GPU memory

High-bit stitching uses fused RGB10A2 unpack/remap by default. Instead of allocating two half4 camera images, hm-cupano unpacks each selected source sample while remapping into FP16 blend scratch. Two 7680 × 4320 cameras save 506.25 MiB. Stitched resolution, all pyramid levels, FP16 rounding, mapping-validity alpha, and exposure/shadow grading remain unchanged; frames remain GPU-resident.

Select the previous path before startup with:

```text
--options=pipeline.hmstitcher.private-properties.fused-rgb10-remap=false
```

The default is true. Both this setting and `compact-workspace` accept true/false or 1/0 and are startup-only. Calibration uses its existing bounded capture path; the unused legacy RGBA8 calibration-staging helper has been removed.

## Explicit memory controls

`pipeline.hmstitcher.private-properties.compact-workspace=true` enables hm-cupano scratch reuse (default false). Full-canvas blending can return borrowed scratch; hmstitcher consumes it on the same CUDA stream through conversion and any rink-mask preparation. Owned fallback output is cached, including minimized blending. Borrowed views never survive a frame or stitcher reload.

`pipeline.hmstitcher.pre-converter-output-buffers=1` reduces the input converter pool. Missing/zero preserves the converter default. The videoprep GObject property `output-pool-extra-buffers` controls growth beyond `num-output-buffers`; its default remains four. Set it to zero for a fixed pool. Exhausted output-pool waits preserve backpressure and respond to shutdown/seek; a low count can still stall a graph whose downstream components retain all buffers, so validate the actual sink graph.

The tested gse-16a Program/FAKE configuration uses:

```text
--options=pipeline.source0.num-extra-surfaces=0
--options=pipeline.source1.num-extra-surfaces=0
--options=pipeline.source0.low-latency-mode=1
--options=pipeline.source1.low-latency-mode=1
--options=pipeline.hmstitcher.num-output-buffers=1
--options=pipeline.hmstitcher.properties.output-pool-extra-buffers=0
--options=pipeline.hmstitcher.pre-converter-output-buffers=1
--options=pipeline.hmplaycropper.num-output-buffers=1
--options=pipeline.hmplaycropper.properties.output-pool-extra-buffers=0
--options=pipeline.hmstitcher.private-properties.compact-workspace=true
--options=pipeline.primary-gie.batch-size=1
--options=stitching.control_point_resolution=2k
```

Detection uses an FP16 batch-one engine and a 64 MiB builder workspace setting. Decoder low latency was tested on HEVC inputs with no B frames; it is not a general recommendation for other recordings. Decoder-required capture surfaces remain allocated. Prepare calibration and the rink mask before the full Program graph so segmentation and detection workspaces do not overlap. The canonical baseline is unchanged.

## Measured workload

A private copy of the exported `~/Videos/gse-16a/hstream-job.sh` was tested on a Quadro RTX 4000 with the existing desktop running. The native canvas was 14,259 × 4,893; the Program crop was 7678 × 4320. Detection/tracking, original grading, audio, scoreboard and telemetry remained enabled. The exported job selects FAKE, so these measurements do not qualify additional render/encode sinks.

The initial fused run completed 60.093 seconds of video, 3,603 consecutive frames and completed EOS telemetry without `CUDA_LAUNCH_BLOCKING`. Peak process VRAM was 5,482 MiB; peak total was 7,351 MiB including desktop, with 327.77 seconds elapsed. That run used the equivalent pool counts before they became explicit controls. Final-code validation is recorded in the PR.

A bounded test is not a whole-recording reliability guarantee. Qualification checks frame continuity, actual processed video duration, geometry, telemetry completion, errors and watchdogs; process exit status alone is insufficient.
