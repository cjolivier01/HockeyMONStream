# Stitching: Two-Pass vs One-Pass Design Notes

This file captures the current behavior of the stitching pipeline and a plan
for moving to a more robust one-pass design using GStreamer caps negotiation.

It is intended as a hand-off note so future work can start from a clear picture
of how things work today and what needs to change.

## Current Two-Pass Stitching Design

Key files:
- `src/gst-plugins/gst-videoprep/stitcher/stitcher.cpp`
- `src/libs/stitching/ConfigureStitching.cpp`
- `src/apps/pipeline-app/configurator.cpp`
- Game configs under `$HOME/Videos/<game-id>/config.yaml`

### Control masks and canvas size

- `ConfigureStitching.cpp` (`hm::stitching::configure_stitching`, `is_stitching_configured`,
  `get_canvas_size`) uses hm-cupano's `ControlMasks` on the **game directory** to:
  - Generate control masks (via Python `hmcreate_control_points` using saved `left.png` / `right.png`).
  - Later read `canvas_width()` / `canvas_height()` from those masks.
- This is outside of the GStreamer pipeline; it is triggered either by:
  - External tools (`scripts/configure_game.sh` → `external/hm/stitch.sh --configure-only`), or
  - The plugin running in a special "configure-only" mode and then forcing EOS.

### hmstitcher plugin behavior

File: `src/gst-plugins/gst-videoprep/stitcher/stitcher.cpp`

- Properties:
  - `config-file` (directory, usually the game dir).
  - `configure-only` (bool).
  - `left-frame-offset-ns` / `right-frame-offset-ns` (applied after sync).
- `StitcherPriv::get_stitcher()`:
  - If `configure_only_ == true`, returns `nullptr` (no stitcher, no canvas info).
  - Otherwise, loads `hm::pano::ControlMasks` from `config_file_` and constructs a `STITCHER`.
- `StitcherPriv::PreCapsInit()`:
  - If a `STITCHER` exists, uses its `canvas_width()` / `canvas_height()` to set:
    - `params->output_width_height[0]`
    - `params->output_width_height[1]`
  - This is how hmstitcher sets its output caps in the normal run.
- `StitcherPriv::GenerateOutput()`:
  - On the first batch, checks `hm::stitching::is_stitching_configured(config_file_)`.
  - If not configured:
    - If `configure_only_ == false`:
      - Returns an error `"Stitching is not configured"` → a plain run pipeline fails.
    - If `configure_only_ == true`:
      - Calls `hm::stitching::configure_stitching(config_file_, left_surface, right_surface)`,
        which runs `hmcreate_control_points` to build control masks.
      - Then posts a pipeline EOS via `post_force_pipeline_eos` to terminate that pipeline.

### Configurator wiring and sizing

File: `src/apps/pipeline-app/configurator.cpp`

- `setup_stitcher_and_masks()`:
  - Always sets `pipeline["hmstitcher"]["config-file"] = game_dir`.
  - If `hmstitcher.enable && hmstitcher.configure-only`:
    - Optionally checks `is_stitching_configured(game_dir)` and short-circuits if masks are up-to-date.
  - Also wires the ds-fieldmask detection mask path into the pipeline.
- `gather_stitching_videos()`:
  - Discovers left/right video chapters for a game (`Orientation.cpp`).
  - If needed, calls `configure_orientation(game_dir)` (Python `hmorientation`) to populate
    `game.videos.left/right` in the private config.
  - Computes `game.stitching.frame_offsets` via `calculate_stitching_synchronization`.
- `apply_frame_offsets_and_sizes()`:
  - Computes frame-based offsets in nanoseconds from `frame_offsets` and the FPS of the input videos.
  - Writes:
    - `pipeline["hmstitcher"]["left-frame-offset-ns"]`
    - `pipeline["hmstitcher"]["right-frame-offset-ns"]`
  - Tracks whether offsets are nonzero (`set_stream_offsets_`).
- `set_output_dimensions()`:
  - If the sources are cameras, sizes are taken from `camera-width` / `camera-height`.
  - If `left_files/right_files` are present and hmstitcher is enabled:
    - Calls `get_canvas_size(game_dir)`:
      - This uses `ControlMasks` to read the canvas width/height from the game dir.
    - On success:
      - Sets `hmstitcher.output-width/height` and `hmplaycropper.output-width/height`.
      - Also sets `streammux.width/height` (possibly scaled for UDP).
    - On failure:
      - If `hmstitcher.configure-only` is `false`, returns a `FailedPreconditionError`:
        - `"Unable to determine the canvas size and stitcher is not set to configure-only"`.
      - If `configure-only` is `true`, logs a message and allows the pipeline to proceed,
        expecting the next run to find the now-known canvas.
  - If no left/right but a pre-stitched file exists, uses its dimensions as a fallback.

### Net effect

Because the plugin and configurator rely on control masks and `ControlMasks::canvas_width/height()`,
the flow is effectively:

1. **Configure-only pipeline**:
   - hmstitcher is enabled with `configure-only=1`.
   - On first frames, `GenerateOutput()` runs `configure_stitching()`, generates control masks, and forces EOS.
2. **Run pipeline**:
   - hmstitcher now finds the control masks; `get_stitcher()` returns a valid `STITCHER`.
   - `PreCapsInit()` sets `output_width_height` from the canvas size.
   - `Configurator::set_output_dimensions()` reads the same canvas size and propagates it to downstream bins.
   - The pipeline runs normally.

This is why today's design requires two pipelines / passes for stitching.

## Target One-Pass Design (High-Level)

Goal: configure stitching (control masks and offsets) and run the stitched pipeline in **one** GStreamer pipeline,
possibly with temporary internal reconfiguration, but without the user having to manage two separate runs.

Key ideas:

1. **Move stitching configuration into hmstitcher without killing the pipeline**:
   - On the first frames:
     - If control masks are missing:
       - Call `configure_stitching(config_file_, left_surface, right_surface)` from inside the plugin.
       - Immediately reload `ControlMasks` and create the `STITCHER` instance.
     - Avoid forcing EOS; instead, reinitialize internal state and continue.
   - This means `configure_only` likely becomes unnecessary or is reinterpreted.

2. **Use caps negotiation (and possibly renegotiation) for sizing**:
   - Once `ControlMasks` are available in the same pipeline lifetime:
     - `PreCapsInit()` (or a later caps-init hook) can set `params->output_width_height` with the real canvas size.
   - Downstream elements (`hmplaycropper`, `streammux`) should:
     - Either rely entirely on caps negotiation, or
     - Query upstream caps (or use pad-probes) to learn the stitched output size, instead of being sized ahead of time in `Configurator`.
   - This may require:
     - Allowing caps renegotiation once the canvas size is known.
     - Or delaying caps fixation until after the stitching configuration pass is complete.

3. **Simplify Configurator**:
   - Remove or reduce:
     - `get_canvas_size()` calls and the strict `FailedPreconditionError` when canvas size is unknown.
     - Hard-coded `hmstitcher.output-width/height` assignments that duplicate what the plugin already knows.
   - Keep:
     - Video discovery and orientation (`gather_stitching_videos`, `Orientation.cpp`).
     - Audio synchronization and frame-offset calculation (`calculate_stitching_synchronization`).
     - Basic pipe wiring (enabling bins, hmaudio setup, UDP scaling where needed).

4. **Maintain compatibility with existing tools**:
   - `scripts/configure_game.sh` and Python tools (`hmorientation`, `hmcreate_control_points`) should continue to work for offline pre-configuration.
   - New in-pipeline configuration must:
     - Respect existing masks when they are already present.
     - Only regenerate them when requested (e.g., via a `force` flag) or if the configuration clearly requires it.

## Suggested Refactor Plan (Later Work)

When picking this up again, a reasonable sequence would be:

1. **Refactor hmstitcher plugin** (`stitcher.cpp`):
   - Change `configure_only_` semantics:
     - Prefer in-pipeline configuration on first frames.
     - Avoid forcing EOS as the normal path.
   - After `configure_stitching` runs:
     - Reload `ControlMasks` and construct `STITCHER` in the same pipeline.
     - Ensure `PreCapsInit` (or equivalent) sees a valid stitcher and sets correct `output_width_height`.
   - Add tests / logging around the first frames where configuration happens.

2. **Update Configurator to trust caps**:
   - Gradually remove the hard dependency on `get_canvas_size()`:
     - Make the "Unable to determine canvas size" error a warning or a fallback path.
   - Add logic (if needed) to query hmstitcher output caps and propagate them to `hmplaycropper` / `streammux`
     if they cannot infer sizes automatically.

3. **De-risk with a feature flag or config knob**:
   - Introduce a config option in the `pipeline` YAML (or a CLI flag) to switch between:
     - Legacy two-pass behavior.
     - Experimental one-pass behavior.
   - This allows you to test on a subset of games without breaking existing workflows.

4. **Clean up:**
   - Once one-pass behavior is stable, deprecate:
     - `hmstitcher.configure-only` and pipeline-level expectations around it.
     - The explicit "configure-only" pipeline path in scripts (if no longer needed).
   - Simplify `Configurator` to focus on staging, orientation, and offsets, not canvas-size discovery.

## How to Use This Note Later

When you come back to this work:

- Search for this file: `docs/stitching_onepass_notes.md`.
- Use the "Current Two-Pass Stitching Design" section as a map of where to look in the code.
- Use the "Target One-Pass Design" + "Suggested Refactor Plan" as a checklist for plugin and configurator changes.

This summary is intentionally high-level so it stays valid even if some details shift as the code evolves.

