# Native-canvas GPU memory

High-bit stitching uses fused RGB10A2 unpack/remap by default. Instead of allocating two half4 camera images, hm-cupano unpacks each selected source sample while remapping into FP16 blend scratch. Two 7680 × 4320 cameras save 506.25 MiB. Stitched resolution, all pyramid levels, FP16 rounding, mapping-validity alpha, and exposure/shadow grading remain unchanged; frames remain GPU-resident.

Select the previous path before startup with:

```text
--options=pipeline.hmstitcher.private-properties.fused-rgb10-remap=false
```

The default is true. Both this setting and `compact-workspace` accept true/false or 1/0 and are startup-only. Calibration uses its existing bounded capture path; the unused legacy RGBA8 calibration-staging helper has been removed.

## Automatic memory profile

The runtime selects the low-memory profile automatically when the selected CUDA device reports 8 GiB or less of total memory. GPUs above 8 GiB retain the configured pool and batch sizes, including 4090- and 5090-class devices. If the CUDA memory query fails, the runtime retains the standard settings.

Set `runtime.gpu_memory_profile` to `auto` (the implicit default), `low`, or `standard` in user/game YAML or through `--options`. `low` forces the profile on any GPU; `standard` disables it on an 8 GiB GPU. Explicit user, game, or CLI values for an individual setting take precedence over the selected profile.

The desktop UI exposes **Normal** and **Low memory** under Program Controls → Runtime. Its initial session choice is Low memory on GPUs with at most 8 GiB and Normal on larger GPUs or when the CUDA query fails. The selection is passed to each CLI run and exported job but is never written to user or game YAML.

The low-memory profile applies the qualified settings below to existing pipeline sections:

```text
pipeline.source*.num-extra-surfaces=0
pipeline.source*.low-latency-mode=1
pipeline.hmstitcher.num-output-buffers=1
pipeline.hmstitcher.properties.output-pool-extra-buffers=0
pipeline.hmstitcher.pre-converter-output-buffers=1
pipeline.hmplaycropper.num-output-buffers=1
pipeline.hmplaycropper.properties.output-pool-extra-buffers=0
pipeline.hmstitcher.private-properties.compact-workspace=true
pipeline.primary-gie.batch-size=1
pipeline.primary-gie.config-file=config_infer_yolov8_hockey_fp16.yaml
```

The resolved run configuration records `runtime.resolved_gpu_memory_profile` and `runtime.detected_gpu_memory_mib` for diagnostics and telemetry provenance.

## Individual memory controls

`pipeline.hmstitcher.private-properties.compact-workspace=true` enables hm-cupano scratch reuse (default false). Full-canvas blending can return borrowed scratch; hmstitcher consumes it on the same CUDA stream through conversion and any rink-mask preparation. Owned fallback output is cached, including minimized blending. Borrowed views never survive a frame or stitcher reload.

`pipeline.hmstitcher.pre-converter-output-buffers=1` reduces the input converter pool. Missing/zero preserves the converter default. The videoprep GObject property `output-pool-extra-buffers` controls growth beyond `num-output-buffers`; its default remains four. Set it to zero for a fixed pool. Exhausted output-pool waits preserve backpressure and respond to shutdown/seek; a low count can still stall a graph whose downstream components retain all buffers, so validate the actual sink graph.

The tested gse-16a Program/FAKE configuration resolves to:

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

The profile replaces the bundled default FP32 detector config with its FP16 counterpart. An explicitly selected detector config, including BF16 or a custom model, takes precedence. Detection uses a batch-one engine and a 64 MiB builder workspace setting. Decoder low latency was tested on HEVC inputs with no B frames. Recordings that require frame reordering can explicitly override `low-latency-mode=0` while retaining the rest of the profile. Decoder-required capture surfaces remain allocated. Prepare calibration and the rink mask before the full Program graph so segmentation and detection workspaces do not overlap. The canonical baseline remains unchanged; the automatic profile is derived after all configuration layers are loaded.

## Measured workload

A private copy of the exported `~/Videos/gse-16a/hstream-job.sh` was tested on a Quadro RTX 4000 with the existing desktop running. The native canvas was 14,259 × 4,893; the Program crop was 7678 × 4320. Detection/tracking, original grading, audio, scoreboard and telemetry remained enabled. The exported job selects FAKE, so these measurements do not qualify additional render/encode sinks.

The initial fused run completed 60.093 seconds of video, 3,603 consecutive frames and completed EOS telemetry without `CUDA_LAUNCH_BLOCKING`. Peak process VRAM was 5,482 MiB; peak total was 7,351 MiB including desktop, with 327.77 seconds elapsed. That run used the equivalent pool counts before they became explicit controls. Final-code validation is recorded in the PR.

A bounded test is not a whole-recording reliability guarantee. Qualification checks frame continuity, actual processed video duration, geometry, telemetry completion, errors and watchdogs; process exit status alone is insufficient.
