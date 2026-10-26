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

Program → Color and Stitched → Color & Precision share the three-state
**10-bit / FP16 mode** checkbox. Click or Space cycles Auto, Force on, Force off,
then Auto. Its default comes from `pipeline.hmstitcher.properties.high-bit-depth`
in `configs/baseline.yaml`; account and game settings retain their usual
precedence. Saving Auto over a forced default preserves an explicit Auto
override. Precision changes apply to the next run; Reset Controls during live
playback preserves that selection. `defaults_preset_test` checks mirrored
cycling, custom baseline modes, saving/reloading Auto and Reset to Baseline;
`tone_controls_window_test` checks live Reset preserves next-run precision.

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

Set `HSTREAM_UI_E2E_CANCEL_INITIAL_SCOREBOARD=1` on a private fixture to exercise
Cancel instead of No scoreboard. With `HSTREAM_UI_E2E_PREPARE_GAME=1`, preparation
clears canonical and legacy scoreboard geometry. Allow at least 12 seconds of
recording: the check requires distinct processed-video positions after Cancel,
no replacement selector, an unchanged empty polygon, and normal process teardown,
in addition to the ordinary rendered-output checks. Initial Cancel continues
playback without a scoreboard; it does not save the No scoreboard sentinel.
Run `HSTREAM_UI_E2E_SCOREBOARD_CONTROLS=1` separately for cancellation/reselection
of an already saved polygon and acknowledged placement changes.

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

For timestamp-qualified comparisons, the explicit runner command
`@capture-preview-frame program /absolute/path/frame.png` also logs a
`runtime preview frame identity` line with the renderer generation and
`pipeline-pts-ns`. This is the rendered buffer's PTS after native playlist/seek
rebasing, not a raw chapter-local timestamp. The image and identity are captured
under the same render lock.
The identity is omitted if the current window, generation or size differs from
the last render, or while the window is hidden. Native WebRTC's accepted
`sourcePts` is also the buffer PTS before transport rebasing. Compare these only
after verifying identical source identities, chapter order, synchronization and
playback start/seek epochs. The alternate HockeyMON decoder's chapter-local PTS
needs an explicit timeline mapping; numeric equality alone cannot identify the
same media frame. An ordinary screenshot has no timestamp evidence.
This uses the existing bounded, one-shot diagnostic readback and adds no video
readback to normal playback. A window that Qt cleared still needs a newly
presented frame before comparison.

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

The separate opt-in GPU tone check runs real Program processing with video
rendering and archive outputs disabled. It compares native typed readbacks
with Qt edits: coalesced shadows/black-point/exposure updates, a newer edit
queued behind genuine delayed acknowledgments, mirrored keyboard changes,
wheel protection, retained disabled black-point state, and Save Preset during
playback. Changing the next-run precision checkbox must preserve the active
native owner and precision before and after Save.

Build `//src/apps/hstream-ui:tone_controls_gpu_test` and the native CLI/plugins
first. Run this command inside the machine's GPU allocation, once with
`standard` and once with `high-bit`, using a different artifact directory:

```bash
python3 scripts/test_hstream_tone_controls_gpu.py \
  --source-game /mnt/games/game-42 --native-root "$PWD" \
  --artifacts /tmp/tone-standard --precision standard
```

The wrapper requires a recorded game with saved calibration (`panorama.tif`),
copies configuration/calibration into the new artifact directory, and symlinks
untouched video inputs. It disables optional calibration dialogs
and telemetry in that copy and isolates Qt preferences. Model caches remain
available from the normal user environment. Logs, native runner SHA-256, and
screenshots stay outside the Git tree. This test verifies tone-control behavior;
it does not establish preview pixel, calibration, or complete workflow parity.

Set `HSTREAM_UI_E2E_SKIP_OPTIONAL_REVIEW=1` for preview-only acceptance when an
older fixture may need calibration regeneration. This disables optional crop
and leveling review in the copied game's settings; it does not change the
source game or skip native calibration. Scoreboard selection remains automated.

`HSTREAM_UI_E2E_PREVIEW_TRANSITIONS_ONLY=1` selects a separate live X11 check:
Program → paused Stitched → Camera 1, a source-only seek, Render off immediately
after starting a backward seek, restart with rendering still off, then Program
reactivation. It preserves Qt's rule that Render off disables new seek requests.
It turns archive routes and telemetry off so native local seeking is available,
uses normal-speed playback, and requires generation-matched readiness plus
nonblank native diagnostic captures. It writes `preview-transitions-report.txt`
and `pipeline.log` in the artifact directory. Run this with `--x11-preview` and
the same copied-game setup; it replaces the archive/visual-matching check, so
run the ordinary archive check separately. A new-process restart is covered;
this does not force the same-process full-reconstruction seek fallback.

The focused CPU regression `HSTREAM_UI_TEST_PREVIEW_OVERLAY_STARTUP_ONLY=1`
checks that overlay changes and Render-on before runtime readiness retain the
latest choices, send no early command, and start acknowledgment timing only
after the native readiness handshake.

With `HSTREAM_UI_E2E_PREVIEW_OVERLAY_CONTROLS=1`, the transition check also
changes Player boxes, Play tracking and Rink mask on Program and Stitched,
requires complete live acknowledgments and captures each combined selection.
It ends each route with all three off. These are diagnostic framebuffer captures;
ordinary playback does not read video back to the CPU.

With `HSTREAM_UI_E2E_LIVE_CAMERA_CONTROLS=1`, the transition check also changes
all five Players drawing layers, pauses before editing tracker controls, and
holds those drafts beyond the runtime acknowledgment timeout. Resume must
confirm the queued controls with their native units before capturing another
frame. Live Reset restores the camera defaults while preserving Drawing.
The focused `tone_controls_window_test` covers the same pause/resume deferral
without a GPU.

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

The complete fake-runner main-window comparison uses a screen large enough for
its resize and composed native-control checks:

```sh
QT_QPA_PLATFORM=xcb xvfb-run -a -s '-screen 0 2560x1600x24' \
  bazel-bin/src/apps/hstream-ui/hstream_ui_test
```

The default 1280×1024 Xvfb screen clips the test's 1500-pixel-wide focused
preview, so its top-right icon cannot appear in the captured image. The Bazel
test uses `QT_QPA_PLATFORM=offscreen` for the non-X11 behavior checks. Set
`HSTREAM_UI_TEST_DESTRUCTION_ONLY=1` to isolate active pipeline/finalizer and
transient-process destruction; it verifies safe signal teardown and process reaping on either
platform. Normal window closing still waits for the pipeline and publication.

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
