# Native-canvas GPU memory

High-bit stitching uses fused RGB10A2 unpack/remap by default. Instead of allocating two half4 camera images, hm-cupano unpacks each selected source sample while remapping into FP16 blend scratch. Two 7680 × 4320 cameras save 506.25 MiB. Stitched resolution, all pyramid levels, FP16 rounding, mapping-validity alpha, and exposure/shadow grading remain unchanged; frames remain GPU-resident.

Select the previous path before startup with:

```text
--options=pipeline.hmstitcher.private-properties.fused-rgb10-remap=false
```

The default is true. The setting accepts true/false or 1/0 and is startup-only. Calibration keeps its existing bounded capture path.

## Measured workload

A private copy of the exported `~/Videos/gse-16a/hstream-job.sh` was tested on a Quadro RTX 4000 with the existing desktop running. The native canvas was 14,259 × 4,893; the Program crop was 7678 × 4320. Detection/tracking, original grading, audio, scoreboard and telemetry remained enabled. The exported job selects FAKE, so these measurements do not qualify additional render/encode sinks.

The initial fused run completed 60.093 seconds of video, 3,603 consecutive frames and completed EOS telemetry without `CUDA_LAUNCH_BLOCKING`. Peak process VRAM was 5,482 MiB; peak total was 7,351 MiB including desktop, with 327.77 seconds elapsed. This end-to-end qualification also enabled separate compact-workspace and buffer-count controls; packed-remap parity is tested independently.

A bounded test is not a whole-recording reliability guarantee. Qualification checks frame continuity, actual processed video duration, geometry, telemetry completion, errors and watchdogs; process exit status alone is insufficient.
