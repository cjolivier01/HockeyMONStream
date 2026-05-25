# Dynamic One-Pass Stitching Plan

## Status

Implemented and verified with the cleaned `chicago-4` one-pass FAKE-sink repro. The one-pass path now defers `hmstitcher` output pool allocation until the generated canvas size is known.

## Problem

Before this change, one-pass stitching started the pipeline before stitching control masks existed. Because `videoprep` negotiated fixed output caps and allocated its output pool before the first batch reached `hmstitcher`, the configurator guessed an initial stitched output surface size from the input videos. That guess could be too small, causing `FAILED_PRECONDITION: Output surface is smaller than expected stitched canvas`, or too large, causing unnecessary CUDA memory pressure.

## Goal

Remove the guessed one-pass `hmstitcher` dimensions. Let the first input batch drive stitching analysis, discover the real canvas size, then publish fixed output caps and allocate output buffers with the actual canvas dimensions before emitting stitched frames downstream.

## Implementation Steps

1. Add an algorithm hook in the `videoprep` custom algorithm base that runs before acquiring an output buffer. The hook can inspect the input batch and request a runtime output size.
2. Implement the hook for `hmstitcher` one-pass mode. On the first unconfigured batch, run orientation and stitching configuration from the input surfaces, reload control masks, and return the actual canvas width and height.
3. Add lazy output-pool creation/reconfiguration in `CustomAlgorithmBase`. If the negotiated size changes before output is pushed, build fixed output caps for the new size, push a CAPS event downstream, and create/recreate the NvBufSurface buffer pool with those caps.
4. Change `videoprep` caps negotiation so one-pass `hmstitcher` can start without fixed output dimensions from YAML. The initial caps query can advertise a range, but the first real CAPS event pushed with buffers must be fixed to the generated canvas size.
5. Audit downstream components (`ds-fieldmask`, `ds-playtracker`, `hmplaycropper`, sinks) for cached dimensions or scratch allocations that assume the initial caps are final. Move any such allocations to `set_caps` or make them reallocate on caps changes.
6. Remove the configurator one-pass fallback dimensions and update `AGENTS.md` once the dynamic path is verified.

## Validation

- Build `//src/apps/pipeline-app:pipeline-app` and relevant `gst-videoprep` tests.
- Run the cleaned one-pass repro:
  `cd ~/Videos/chicago-4 && ../clean.sh && cd - && ./run.sh --one-pass-only --game-id=chicago-4 --enable-sinks=FAKE -t 2`
- Confirm the run no longer logs guessed `hmstitcher` fallback dimensions and does not emit `Output surface is smaller than expected stitched canvas`.
- Re-run a normal already-configured game to verify the existing fixed-canvas path still negotiates normally.
