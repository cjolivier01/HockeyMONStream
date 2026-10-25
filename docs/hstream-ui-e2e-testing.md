# HStream UI end-to-end acceptance test

The Game panel shows the current **Game root** (the folder containing game
directories). **Change root…** chooses another folder and reloads its games;
it is disabled during playback and archive finalization. The UI remembers the
selection in `~/.config/hstream-ui/settings.ini`, or
`$XDG_CONFIG_HOME/hstream-ui/settings.ini` when set. At startup, `HM_GAME_DIR`
takes precedence over this preference, followed by `paths.game-root` in the
shared `~/.hstream/hstream.yaml` configuration and finally `~/Videos`.
An explicit choice in the UI applies immediately to that window and its runner.

The **Existing** dropdown also contains **Defaults**. Selecting it loads the
bundled `configs/baseline.yaml` overlaid by `~/.config/hstream-ui/defaults.yaml`,
without a game's settings or recording metadata. **Save Preset** writes that
user defaults file; selecting a game keeps saving to its `config.yaml`.
The application never writes the bundled baseline. The defaults path is fixed
under `HOME`, independently of the `XDG_CONFIG_HOME` UI preference location.

**Reset to Baseline** appears only for Defaults. It loads the baseline into the
controls without changing files. Save Preset then writes an empty YAML map (`{}`)
if no further edits were made; edits after Reset are saved as new overrides.
Playback and game-specific actions require selecting a game.

Both NVIDIA and AMD loaders apply baseline, UI defaults, and game configuration
in that order, followed by explicit launch overrides. Until defaults.yaml exists,
legacy tuning in `~/.hstream/hstream.yaml` remains supported. Saving Defaults
opts into the new tuning file, including when Reset writes `{}`; legacy tuning
is then ignored so it cannot override new settings through native aliases or
reappear after Reset. Legacy `paths` preferences remain supported and the legacy
file is never modified. Relative native references resolve beside the active
user tuning file. The Defaults editor always shows baseline plus defaults. Invalid defaults fail with their source path
instead of silently falling back. Native tracker tuning uses immutable generated
tracker files alongside the defaults, referenced by absolute path so it works
for every game; these are never written into the bundled configuration.

Focused checks: `bazelisk test //src/apps/hstream-ui:defaults_preset_test
//src/apps/hstream-cli:configurator_persistence_test
//src/libs/program:launch_config_test` covers selection, save/reopen, game
precedence, color and calibration controls, reset/save, baseline preservation,
and the shared NVIDIA/AMD configuration paths.

In Video Sets, **Browse** selects one or more files for the chosen camera role.
**Add** imports that selection (or a typed path); with no selection it opens the
same multi-file browser. Successful imports clear the selection. Both buttons
reopen the last selected video folder, which is also remembered across UI runs.
Cancel leaves the selection and remembered folder unchanged. Failed imports
remain selected for retry; successfully imported files are retained.

Focused checks: `QT_QPA_PLATFORM=offscreen HSTREAM_UI_TEST_PATHS_ONLY=1
bazel-bin/src/apps/hstream-ui/hstream_ui_test` covers root switching and
persistence, environment precedence, and multi-file selection/import/reopening.

The UI acceptance test exercises the same Qt button handlers as an operator:

1. load the selected game;
2. configure display rendering and select the requested archive checkboxes;
3. click **Play**;
4. observe the private native scoreboard-selector URL and submit **No scoreboard**;
5. wait for positive pipeline FPS while recording several seconds;
6. click **Stop** and inspect the working MKV, or wait for completion and require published MP4s;
7. compare sampled encoded frames with `panorama.tif` using SIFT features and a RANSAC homography.

Positive FPS alone is deliberately insufficient. The visual check requires square pixels and at
least 20 geometrically consistent feature matches and a 35% inlier fraction,
so a healthy pipeline processing the wrong game fails.

The **Play at normal speed** checkbox beside **Render video** is an opt-in,
session-only playback choice. Select it before Play; it is disabled when rendering
is off or a run is active. The runner's `--render-realtime` option clocks local
video and audio from their timestamps, disables preview frame sampling/leaky
queues, and retains late video frames (`max-lateness=-1`, `qos=false`). The render
terminator prerolls before the playback clock starts. Calibration runs with the
unrestricted monitor policy so collecting frame pairs and crossing URI chapters
cannot wait on output preroll. After calibration, playback recreates the graph at
the requested start (including zero) and enables pacing. Hidden previews remain gated.
Slow processing can make playback stutter; it does not trigger frame skipping.

The setting does not alter archive frames, timestamps, audio samples, encoding
settings, or exported jobs. Backpressure does limit encoding throughput to playback
speed. Pacing stays fixed for the run, including while Render video temporarily
hides/mutes the monitor; Stop allows choosing unrestricted processing again.
Playback uses the negotiated media timeline and does not select or convert mixed
camera frame rates.

Focused checks: run `uri_multiple_pipeline_test --render-realtime-only` for clock
pacing, calibration preroll, slow processing, unchanged output timestamps and
monitor-audio policy; `stitch_frame_time_plan_test` covers calibration restart at zero;
run `hstream_ui_test` with `HSTREAM_UI_TEST_REALTIME_ONLY=1` for checkbox gating
and launch/export arguments.

For a fast UI-only regression run, the X11 interaction test uses real Qt mouse
events against native XCB widgets under a window manager. It verifies native
parent, geometry, and map state while exercising stopped double-click, live
focus/restore, resize, render-off while focused or paused, missing render-off
acknowledgement recovery, active and unrelated preview failures,
stop-while-focused, and live render off/on. It also
saves composed X11 screenshots for inspection:

```bash
scripts/test_hstream_ui_x11_interactions.sh
```

## Exported encoding jobs

**Save job script** invokes `hstream-job`. Jobs disable display and network sinks and
encode the checked Program, Stitched, and/or 4K Program outputs. With no file output
checked, they encode the 4K Program copy. Stitched-only exports use the stitching-only
pipeline, skipping detection, rink filtering, player analytics, play tracking, Program
cropping and DriveGPT capture, even when Program mode is selected. Any Program archive
uses the full Program pipeline. In Stitching Calibration mode, only Archive Stitched is
valid: Program selections (including the no-selection 4K default) show an error in
hstream-ui before the save dialogs or any settings are written. Switch to Program mode
to change the disabled Program archive checkboxes. The Render video checkbox still
controls local Play.

The generated script invokes `hstream-finalize`, which supervises `hstream-cli --encode-job`,
requires its completed run manifest, publishes faststart MP4s and telemetry in the game
directory, and removes owned working files only after all required outputs are saved.
Both commands come from the execution host's `PATH`, including for scripts exported
from a development UI. The UI does not export its local CLI, config, working directory
or baseline override. The selected CLI finds its own bundled config and runtime;
the script retains game settings and the selected working output location.
Each export has a fixed run ID: repeating its script resumes finalization/cleanup without
encoding again; export a new script for another run. Custom working output paths remain
supported. Stitched jobs retain their independent output path and bitrate. Direct
`hstream-cli --encode-job` without the managed wrapper still writes MP4 in working storage
without publication or faststart. See [the lifecycle](../LIFECYCLE.md) for staging, receipts,
failure recovery and cleanup.

DriveGPT capture defaults on for Program jobs. Export freezes the current checkbox choice in the script,
including an explicit off choice, so later UI saves do not change it. Old recipes without
that field use the configured DriveGPT default.
Standalone `hstream-job --game-dir DIR` also defaults to 4K when the recipe has no file
encoder; `--enable-sinks ENCODE_FILE,ENCODE_STITCHED_FILE` overrides its archive selection.
Stitched-only selections use the same short pipeline for standalone exports; calibration-only
recipes that request Program archives are rejected.
Encoding jobs require a YAML pipeline config; legacy DeepStream `.txt` configs are rejected.

Quick export checks:

```bash
bazelisk test --config=opt --cpu=k8 //src/apps/hstream-job:job_script_test
QT_QPA_PLATFORM=offscreen HSTREAM_UI_TEST_JOB_EXPORT_ONLY=1 \
  bazel-bin/src/apps/hstream-ui/hstream_ui_test
```

The UI check covers all eight archive checkbox combinations in both run modes, exporting
valid scripts through the real tool and checking that invalid combinations show a popup
without writing settings or a script. It also checks pipeline selection, rendering left
selected, and telemetry capture.
The script test executes generated jobs against a recording runner and checks sink
selection, argument quoting, exit status and failure without overwriting an existing job.

## Run it

```bash
scripts/test_hstream_ui_e2e.sh short
```

On an x86_64 workstation with an active NVIDIA X11 display, add
`--x11-preview` to keep live rendering enabled. This variant clicks the
**Program**, **Stitched**, and **Camera 1** tabs, requires native render-target
acknowledgements, captures the Qt-painted preview surfaces, and fails if the
Program or Stitched surface is blank:

```bash
DISPLAY=:0 scripts/test_hstream_ui_e2e.sh --x11-preview short
```

`HM_GAME_DIR` selects the source game root, as it does for the application. An
optional second argument selects the artifact directory:

```bash
HM_GAME_DIR=/mnt/games scripts/test_hstream_ui_e2e.sh game-42 /tmp/hstream-ui-game-42
```

The script builds the x86_64 optimized UI, real CLI backend, and visual
verifier. It creates a sandbox game directory whose video inputs are symlinks
to the source game. Configured stitching artifacts are reflinked (or copied
when reflinks are not available), while `config.yaml`, UI state, locks, and
encoded output remain isolated. The source game is not modified.

On X11, the backend renders NVMM video through CUDA/OpenGL into the native
preview windows. The acceptance test requests bounded diagnostic captures of
the presented GPU framebuffer; ordinary playback performs no CPU frame readback.
The test requires distinct channel acknowledgements and non-blank images, then
checks the recorded working MKV after Stop. With
`HSTREAM_UI_E2E_WAIT_FOR_EOS=1`, it waits for natural completion and requires all
selected outputs to finish MP4 publication. It selects the CLI
and plugins from the same Bazel build as the test executable.

`HSTREAM_UI_E2E_OUTPUTS` selects comma-separated `archive-file`,
`archive-stitched`, and `archive-program-4k` checkboxes (default: `archive-file`).
`HSTREAM_UI_E2E_RUN_MODE=stitch-calibration` exercises stitched-only playback
with `HSTREAM_UI_E2E_OUTPUTS=archive-stitched`; the omitted Program preview is
excluded from that mode's checks. Set `HSTREAM_UI_E2E_STANDALONE_TELEMETRY=1`
with 4K-only output and `HSTREAM_UI_E2E_WAIT_FOR_EOS=1` to enable DriveGPT
and require its independent database publication after video completion.

## Evidence retained

The artifact directory contains:

- `pipeline.log`: the complete, untruncated UI/backend log;
- `log-issues.txt`: warning/error/critical lines extracted for quick triage;
- `ui-running.png` and `ui-stopped.png`: Qt window screenshots;
- `program-preview-surface.png`, `stitched-preview-surface.png`,
  `camera1-preview-surface.png`, and `preview-report.txt`: live X11 evidence
  when `--x11-preview` is used;
- `encoded-output/`: working archives; finalized MP4s are published in the sandbox game directory;
- `encoded-frame.jpg`: the best sampled output frame;
- `panorama-reference.jpg`: a review-sized panorama;
- `panorama-feature-matches.jpg`: inlier feature links between output and panorama;
- `report.txt`, `visual-report.txt`, and `visual-verifier.log`: machine-readable pass/fail evidence and match counts.

The displayed Qt preview cannot be embedded when the test uses the offscreen
platform, so the encoded archive is the visual ground truth. Scoreboard
selection uses a modal Qt dialog in hstream-ui; the E2E test exercises its
native **No Scoreboard** action when scoreboard configuration is required.

For unusually sparse footage, `HSTREAM_UI_E2E_MIN_VISUAL_INLIERS` can adjust
the default of 20. Lowering it should be accompanied by inspection of
`panorama-feature-matches.jpg`; the 35% inlier-fraction requirement remains.
`HSTREAM_UI_E2E_RECORD_MS` can extend the post-FPS recording interval; the
default is 6000 ms.

The focused native background regression runs with
`QT_QPA_PLATFORM=xcb xvfb-run -a bazel-bin/src/apps/hstream-ui/native_preview_surface_test`.
It paints stale native pixels, grows and shrinks the preview without new video,
and verifies a black background after resize/remap. It also checks that ordinary
Qt updates preserve renderer pixels and resize does not deliver posted events.
A resized process-paused preview remains black until playback resumes; the
runner cannot redraw while SIGSTOP-paused.

The hidden-target sizing regression runs with
`QT_QPA_PLATFORM=xcb HSTREAM_UI_TEST_PREVIEW_GEOMETRY_ONLY=1 xvfb-run -a bazel-bin/src/apps/hstream-ui/hstream_ui_test`.
It resizes a hidden native target, simulates a delayed configure event restoring
its initial size, and checks that showing Program fills the letterboxed surface
without resizing the main window or draining posted events. Live X11 captures
also check the selected target's Qt and native geometry before reading the
diagnostic image.

### Reviewed rink mini-map

With a saved fitted `rink-map.yaml` in an isolated game, set
`HSTREAM_UI_E2E_RINK_MAP=1` and run `scripts/test_hstream_ui_e2e.sh --x11-preview`.
This enables the next-playback mini-map, disables player boxes, and requires
`HSTREAM_RINK_MAP status=ready` from the real runner. Inspect the Program and
Stitched preview captures for the rink and player circles matching detection-box colours.
`HSTREAM_UI_E2E_RINK_MAP_SIZE=large` and
`HSTREAM_UI_E2E_RINK_MAP_POSITION=upper-left` exercise the next-playback display
preferences (also accepts `compact`/`standard` and `lower-left`). The fixture
must match the copied game's calibration; partial/stale maps fail preparation.
The ordinary archive geometry check still runs, since the map is preview-only.
