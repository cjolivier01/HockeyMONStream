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

## Projection-aware calibration

Fresh calibration selects `stitching.mapping_backend` and `stitching.projection` before mapping TIFF generation. With
`nona`, Hugin optimizes the camera alignment, converts the unpublished PTO directly to the selected output projection,
generates both camera remap sets in one mapping phase, and runs `enblend` once. The existing bounds-safety logic may
rerender that same selected projection at a smaller scale when TIFF placement rounding exceeds the canvas; it does not
create or consume an intermediate stitched video frame. The normal downstream rink inference therefore sees the final
projected stitch; calibration no longer creates an ordinary panorama for a separate first rink detection or performs a
second projection/remap/stitch stage.

`nona` supports every Hugin projection exposed by the UI. The native `opencv-magsac` and
`opencv-affine-ransac` mapping backends currently support rectilinear output only. Incompatible YAML pairs fail
validation, while the UI disables non-rectilinear choices whenever an OpenCV backend is selected. Older OpenCV
overrides that did not store a projection migrate to rectilinear.

Parameterized Hugin projections store their values by canonical projection name under
`stitching.projection_parameters`, so switching the UI projection does not discard another projection's tuning.
General Panini (`f19`) exposes `Cmpr`, `Tops`, and `Bots`, defaulting to `[100, 0, 0]`. Albers equal-area conic,
Biplane, and Triplane expose the parameter sets reported by libpano; the other Panini variants do not advertise
adjustable parameters. The UI shows only the controls supported by the selected projection and supplies the libpano
range, default, and behavior description on hover.

Hugin recalculates the projection-aware field of view, canvas, and largest all-image crop so the published remaps do
not retain the black hourglass-shaped region produced by forcing Panini into the old rectilinear canvas. The crop is
scaled down when necessary so it never exceeds the original calibrated canvas or configured live canvas limits.
Steady-state video remains on the existing GPU remap and stitch path.

## Validation

- Build `//src/apps/pipeline-app:pipeline-app` and relevant `gst-videoprep` tests.
- Run the cleaned one-pass repro:
  `cd ~/Videos/chicago-4 && ../clean.sh && cd - && ./run.sh --one-pass-only --game-id=chicago-4 --enable-sinks=FAKE -t 2`
- Confirm the run no longer logs guessed `hmstitcher` fallback dimensions and does not emit `Output surface is smaller than expected stitched canvas`.
- Re-run a normal already-configured game to verify the existing fixed-canvas path still negotiates normally.
