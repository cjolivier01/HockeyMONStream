# Rink mask frame time

The final stitched ice mask can use a clearer frame selected independently of
camera alignment inputs and Playback start. Every game defaults to one second
before the synchronized recording ends, because the first stitched frame is
usually warmup with the ice full of players. A venue or a game can name a
different frame, including the first stitched one.

## Configuration and UI

```yaml
stitching:
  rink_configs:
    tv-dublin:
      display_name: "TV Dublin"
      rotation_degrees: [0, -23.5, 3.23]
      # Optional: a venue whose last second is not the clearest frame.
      rink_mask_frame_time: "auto"
  rink_config: "tv-dublin"
  rink_mask_frame_time: null
  rink_mask_frame_time_default: "-00:00:01"
```

`rink_mask_frame_time_default` is what a game inherits when neither it nor its
venue names a frame, and the baseline is the only place it is written: there is
no compiled-in fallback, and a missing or unparseable value fails rather than
quietly selecting another frame.

In **Stitched → Rink**, **Ice mask frame** offers **Rink default**,
**Automatic**, and **Custom time**. The inherited value appears below the
control, labelled **Rink default** when the selected venue names the frame and
**Default** when it comes from the baseline. Custom times are saved in the
canonical `[-]HH:MM:SS[.mmm]` spelling, so a typed `-1` reloads as `-00:00:01`.
Saving a new time invalidates the mask and dependent rink/scoreboard
geometry while preserving camera alignment, maps, seam, `panorama.tif`, and
retained calibration pairs. The next run prepares the replacement mask.

| Value | Meaning |
| --- | --- |
| Absent or `null` | Inherit the selected rink's value, then `rink_mask_frame_time_default` |
| `auto` | Use the first available stitched frame, overriding both defaults |
| `00:00:00` | Synchronized recording start, even if Playback start is later |
| `10:00` | Ten minutes from synchronized recording start |
| `-1` | One second before synchronized recording end |

Times are `[-][[HH:]MM:]SS[.mmm]`, the same shorthand highlight cues use, so
`-1`, `-0:01` and `-00:00:01` are the same second. Seconds after a colon and
minutes in `HH:MM:SS` must be below 60. Negative zero in any spelling (`-0`,
`-00:00:00`, `-0.000`) names the recording end, where no frame exists, and is
rejected along with invalid syntax and positions
outside the common recording interval. The end is the earliest
camera end after synchronization offsets, summed across all chapters. Playback
start, run limits, and scan windows do not shorten this interval. Frame seeking
uses the existing chapter-aware paired decoder; frame timestamps have the
recording's normal frame granularity.

Normal baseline → user → game → CLI precedence applies. A game-level `null`
selects rink inheritance even over a user-level custom time. CLI example:

```bash
hstream-cli -g GAME --enable-sources=URI-MULTIPLE --enable-sinks=FAKE \
  --options=stitching.rink_mask_frame_time=auto
```

A game that was calibrated before this default existed has a mask saved with no
selection, which no longer matches the inherited one. Its next run prepares a
replacement mask at the new frame; alignment, maps, seam and calibration pairs
survive. Sources that cannot be sampled at a time — anything other than two
recorded URI-MULTIPLE cameras with known durations — need
`stitching.rink_mask_frame_time: auto` before they can build a mask.

## Where a timed frame cannot be reached

Only the startup preparation below can sample a chosen frame. Every other path
that needs a new mask builds it from the frame in hand, and `create_field_mask`
rejects that rather than saving a mask from the wrong moment. Since the shipped
default is now a time, these two cases reach every game instead of only the
venues that chose one:

- Changing the final stitched-output rotation during playback invalidates the
  mask's output generation. The running stitcher rebuilds the mask from the
  current frame and publication fails with "The selected rink mask time
  requires a mask preparation pass; restart playback", which ends the run.
  Restart to rebuild at the configured frame, or select Automatic for that
  game.
- A stitching-experiment baseline prepared before the selection changed keeps a
  mask the player-frame scan cannot load, and that scan deliberately creates no
  mask (`PlayerFrameScan::Create`). Prepare the baseline again, or select
  Automatic for the source game, before resuming a queued player-selection
  candidate.

Closing these would mean letting a running graph schedule its own timed
sampling pass, which is a larger change than the selection itself.

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
provenance. Masks saved without a selection stay reusable only where the
effective selection is `auto`. Changes
through the configured default, profiles, game YAML, or CLI therefore also
require a new mask without requiring another alignment solve. Publication checks the selection again
under the existing configuration lock to reject superseded work.

Focused tests cover parsing/inheritance, UI save/reload and mask-only
invalidation, legacy/malformed provenance, and GPU lifecycle selection/reuse.
The synthetic GPU fixture uses AKAZE/OpenCV mapping and a deterministic test
rink profile; it exercises actual decoding, GPU stitching, and publication
without requiring rink segmentation model inference.
