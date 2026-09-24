# Rink mask frame time

The final stitched ice mask can use a clearer frame selected independently of
camera alignment inputs and Playback start. TV Dublin defaults to one second
before the synchronized recording ends. Other rinks keep the first available
stitched frame unless configured otherwise.

## Configuration and UI

```yaml
stitching:
  rink_configs:
    tv-dublin:
      display_name: "TV Dublin"
      rotation_degrees: [0, -23.5, 3.23]
      rink_mask_frame_time: "-00:00:01"
  rink_config: "tv-dublin"
  rink_mask_frame_time: null
```

In **Stitched → Rink**, **Ice mask frame** offers **Rink default**,
**Automatic**, and **Custom time**. The inherited value appears below the
control. Saving a new time invalidates the mask and dependent rink/scoreboard
geometry while preserving camera alignment, maps, seam, `panorama.tif`, and
retained calibration pairs. The next run prepares the replacement mask.

| Value | Meaning |
| --- | --- |
| Absent or `null` | Inherit the selected rink's value, or automatic if it has none |
| `auto` | Use the first available stitched frame, overriding the rink default |
| `00:00:00` | Synchronized recording start, even if Playback start is later |
| `00:10:00` | Ten minutes from synchronized recording start |
| `-00:00:01` | One second before synchronized recording end |

Custom times use `[-]HH:MM:SS[.mmm]`. Negative zero, invalid syntax, and positions
outside the common recording interval fail explicitly. The end is the earliest
camera end after synchronization offsets, summed across all chapters. Playback
start, run limits, and scan windows do not shorten this interval. Frame seeking
uses the existing chapter-aware paired decoder; frame timestamps have the
recording's normal frame granularity.

Normal baseline → user → game → CLI precedence applies. A game-level `null`
selects rink inheritance even over a user-level custom time. CLI example:

```bash
hstream-cli -g GAME --enable-sources=URI-MULTIPLE --enable-sinks=FAKE \
  --options=stitching.rink_mask_frame_time=-00:00:01
```

The shared baseline remains byte-for-byte synchronized with HockeyMON. This is
a native HStream runtime setting; the Python producer does not sample this time.

## Startup and ownership

`GameConfig.*` resolves profile defaults and direct overrides, preserving their
layer intent when materializing effective worker configuration.
`RinkMaskFrameTime.*` parses, normalizes, and resolves signed times.
`Configurator` distinguishes missing alignment from missing mask work.

When an explicit time requires a new mask, `PipelineApplication` uses bounded
preparation phases within its existing stage lifecycle:

1. Prepare alignment/reframe at its existing calibration inputs if needed,
   deferring final-mask generation.
2. Stop/drain that graph, reload configuration, and run a mask-only graph at
   the resolved recording position using the published GPU maps and seam.
3. Stop/drain mask preparation, reload configuration, then run normal playback
   from the requested Playback start.

This reuses the calibration graph and paired-source positioning also used by
stitching experiments. `StitchingCalibrationMode.h` configures the preparation
as GPU `hmstitcher → FAKE`, with no detector, tracking, audio, preview, or archive
branches. Only the existing bounded segmentation readback occurs. The normal
video path gains no frame copies. Preparation does not consume timed playback.

Each pass has a fresh run generation. Existing completion scopes, cancellation,
worker teardown, and artifact locks fence transitions. Alignment completion
does not report overall calibration complete before the mask is published.
After preparation, the runner acknowledges playback restart only once the new
normal pipeline generation is observed in PLAYING; this closes the UI
calibration dialog without mistaking a preparation graph for Program playback.
Calibration-only runs that omit ice-mask generation retain that behavior.

Explicit sampling currently requires one pipeline context and exactly two
URI-MULTIPLE recorded camera sources with known durations. Unsupported sources
fail rather than silently selecting another frame. Automatic mode retains the
existing source support. Saved compatible masks can be reused without probing
media duration.

## Persistence and validation

`ConfigureStitching` publishes `rink.mask_frame` with the mask transaction:

- `selection`: normalized requested time or `auto`.
- `recording_time_ns`: resolved common position for explicit selections.
- `sources`: both physical chapter paths and camera-local seconds.

The mask remains tied to its existing stitched-output generation. Reuse checks
the effective selection, and explicit selections require complete sampled-frame
provenance. Legacy masks remain compatible with automatic selection. Changes
through profiles, game YAML, or CLI therefore also require a new mask without
requiring another alignment solve. Publication checks the selection again
under the existing configuration lock to reject superseded work.

Focused tests cover parsing/inheritance, UI save/reload and mask-only
invalidation, legacy/malformed provenance, and GPU lifecycle selection/reuse.
The synthetic GPU fixture uses AKAZE/OpenCV mapping and a deterministic test
rink profile; it exercises actual decoding, GPU stitching, and publication
without requiring rink segmentation model inference.
