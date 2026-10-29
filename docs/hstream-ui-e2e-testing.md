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

For a genuinely fresh Calibration workflow, run the built `hstream_ui_test`
directly with `HSTREAM_UI_E2E_CALIBRATION_COMPLETION_ONLY=1`,
`HSTREAM_UI_E2E_FRESH_CALIBRATION=1` and
`HSTREAM_UI_E2E_RUN_MODE=stitch-calibration`. Supply a new game ID through
`HSTREAM_UI_E2E_GAME_ID`, a private game root through `HM_GAME_DIR`, original
recording paths through `HSTREAM_UI_E2E_FRESH_LEFT` and
`HSTREAM_UI_E2E_FRESH_RIGHT`, an explicit `HSTREAM_UI_E2E_REFERENCE_MS` and a new
`HSTREAM_UI_E2E_ARTIFACT_DIR`. The test imports through the Left/Right controls,
retains baseline sampling, performs Skip leveling and Full canvas/Use crop,
checks the native rendered preview and requires ordinary Stop with process
retirement. It records controls, rendered dialogs, configuration and logs.
Optionally set `HSTREAM_UI_E2E_CAMERA_PROFILE` to an exact native profile ID.
The fresh test selects that existing camera control after importing both sources,
records its resolved optics, and requires the completed calibration to retain
those exact values and Auto policies. Unknown IDs fail instead of falling back;
omitting the variable preserves the ordinary default-selection case. Use the
same source bytes and profile in the browser's `HM_STREAM_FRESH_CAMERA_PROFILE`
case. For excerpts without vendor metadata, verify the profile against the
original recordings before choosing it; a successful solve does not establish
physical rink-map accuracy.

Run `HSTREAM_UI_E2E_CALIBRATION_SEEKS_ONLY=1` against the genuinely completed
game to check forward/backward seeking and two queued +10-second clicks during
Pause, followed by one seek on Resume and normal Stop. A recreated source graph
restarts its local frame PTS at zero: check direction on the acknowledged global
playback position, and advancing presented frames within each local epoch.
Both cases reject `HSTREAM_UI_E2E_PREPARE_GAME`; the archive wrapper's synthetic
fixture preparation cannot establish calibration workflow behavior. Keep the
CLI, UI and plugins from the same build and route test audio to an explicitly
owned virtual sink. HockeyMONWeb's matching fresh and saved Calibration tests
check authenticated adoption and actual decoded browser frames.

For the focused Drawing/source-seek regression, use
`HSTREAM_UI_E2E_EFFECTS_OBSERVE_ONLY=1` and an explicit
`HSTREAM_UI_E2E_EFFECTS_PRECISION=0` (Standard) or `1` (FP16). This mode disables
framing, mini-map and diagnostic overlays, records the five Drawing controls,
applies masks 0 then 31, and verifies that a paused Pose edit remains a draft
until Resume applies mask 29. It returns from a source-only seek to Stitched
and requires two advancing, different images, then ordinary Stop and process
retirement. It rejects synthetic fixture preparation and the unrelated live
camera-tuning/Reset mode. It does not qualify tone effects by itself.

Physical-frame comparisons can opt into `HSTREAM_PREVIEW_CAPTURE_METADATA=1`
for Qt's existing explicit capture command. Enable
`pipeline.hmstitcher.private-properties.emit-frame-pair-meta=1` only in the
disposable evidence configuration. `HSTREAM_PREVIEW_CAPTURE` records both
original chapter URIs/PTS and the frame's complete Program crop transform,
latched with the rendered generation. No video readback occurs outside the
explicit capture. Browser native captures instead use a literal JSON
`HSTREAM_PREVIEW_CAPTURE_PLAN` with version 1 and 1–8 requests. Each request
contains `id`, `channel` (`program` or `stitched`), `leftUri`, `rightUri`, exact
decimal-string `leftPtsNs`/`rightPtsNs`, and `count` (1–3). Its process-wide
budget survives preview reconstruction; malformed plans fail before runtime
initialization, and omitted plans add no metadata sampling.

A browser native receipt proves a prepared frame. Join its exact run, stage,
topology, operation, channel, sequence and transport PTS to verified browser
presentation and an explicitly captured image before claiming displayed pixels.
Compare bound source file hashes and original physical PTS across interfaces;
staging URI strings and local playback PTS can differ. Tone comparisons also
require equal finite crop geometry, actual native descriptors/acknowledgments,
and repeated-neutral images to measure capture noise. Missing or mismatched
frames remain incomplete evidence. These diagnostics currently use the NVIDIA
preview implementation; they do not establish AMD or compositor scanout parity.

For preview-only mini-map qualification, use `HSTREAM_UI_E2E_RINK_MAP_ONLY=1`
and `HSTREAM_UI_E2E_RINK_MAP_EXPECTED=ready` or `unavailable`, with a genuinely
completed game and a new evidence directory. This also rejects
`HSTREAM_UI_E2E_PREPARE_GAME`. Optional `HSTREAM_UI_E2E_RINK_MAP_SIZE` and
`HSTREAM_UI_E2E_RINK_MAP_POSITION` select the ordinary next-Play controls.
The test disables archives, telemetry, encoded maps and rink-aware framing,
leaves Player boxes/Play tracking/Rink mask off, and requires current-run
availability plus advancing nonblank, changing Program and Stitched captures.
It checks locked preferences, reopens the Stitched Rink drawer to verify the
visible availability explanation, and checks the source-view explanation and
normal Stop with process retirement. Inspect the ready captures for player dots and `LOCATED`
counts while Player boxes remain off: a matching map can report ready while
waiting for tracks, so readiness alone cannot qualify the snapshot producer.
Keep unavailable-map evidence separate; its video must continue normally.

`HSTREAM_UI_E2E_AV_TIMING_ONLY=1` uses the same genuine saved-game and isolated
evidence requirements for a Program audio/video timing schedule. It enables
Render and normal speed, starts at zero, disables mini-maps/framing and file or
telemetry outputs, and restores the required baseline diagnostic-overlay choices.
After two advancing, changing startup captures, it plays eight seconds, then
performs three three-second pauses with resume windows of eight, eight and twelve
seconds. No seeks, route changes or diagnostic captures occur within those
measured windows. `av-timing-events.jsonl` records Linux `CLOCK_MONOTONIC`
nanoseconds at clicks, matching state acknowledgments and window boundaries,
plus the QProcess PID/arguments and renderer identity. Initial capture intervals
are explicitly marked so external timing analysis can exclude their overhead.
The test requires normal Stop and process retirement; source-correlated audio
and actual presentation timing require a separate owned-sink/metadata collector.

The UI acceptance test exercises the same Qt button handlers as an operator:

1. load the selected game;
2. configure display rendering and select the requested archive checkboxes;
3. click **Play**;
4. observe the private native scoreboard-selector URL and submit **No scoreboard**;
5. wait for positive pipeline FPS while recording several seconds;
6. click **Stop**, or wait for natural completion, and require every selected archive to publish as MP4;
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
requires all selected outputs to finish MP4 publication after Stop. With
`HSTREAM_UI_E2E_WAIT_FOR_EOS=1`, it waits for natural completion before the same
publication checks. Ready playback finishes through the native `q` command;
Stop during startup, pending calibration or seek reconstruction still cancels.
It selects the CLI
and plugins from the same Bazel build as the test executable.

`HSTREAM_UI_E2E_OUTPUTS` selects comma-separated `archive-file`,
`archive-stitched`, and `archive-program-4k` checkboxes (default: `archive-file`).
`HSTREAM_UI_E2E_RUN_MODE=stitch-calibration` exercises stitched-only playback
with `HSTREAM_UI_E2E_OUTPUTS=archive-stitched`; the omitted Program preview is
excluded from that mode's checks. Set `HSTREAM_UI_E2E_STANDALONE_TELEMETRY=1`
with 4K-only output and `HSTREAM_UI_E2E_WAIT_FOR_EOS=1` to enable DriveGPT
and require its independent database publication after video completion.
For telemetry without a video archive, set `HSTREAM_UI_E2E_OUTPUTS=''`,
`HSTREAM_UI_E2E_STANDALONE_TELEMETRY=1` and
`HSTREAM_UI_E2E_VERIFY_X11_PREVIEW=1` in Program mode. This case requires two
changing, nonblank native Program captures with advancing current-generation
timestamps, alongside the Stitched and Camera 1 captures. The archive comparison
is explicitly omitted; its absence cannot substitute for a completed telemetry
receipt. Other archive cases retain the encoded-video checks.
The test captures the current managed run's receipt identity immediately after
Play. Stop and EOS both require that exact receipt to reach `complete` and its
published telemetry database to match the recorded file identity; log messages
and pending receipts do not satisfy publication.
`HSTREAM_UI_TEST_TELEMETRY_RECEIPT_ONLY=1` runs the CPU acceptance fixtures without
launching a window or pipeline.

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

Interactive Program playback with only DriveGPT telemetry must use the same managed
publication lifecycle as a run with video archives. `hstream-job
--interactive-publication` accepts this case only when the saved recipe explicitly
sets `hstream_ui.job.capture_telemetry: true`; it preserves the selected live sinks
and does not add an encoder. Stop must publish a completed telemetry database and
its native receipt before releasing the run. Calibration-only and experiment
preview sessions remain excluded from telemetry capture.

A real saved-leveling follow-up can use `HSTREAM_UI_E2E_SAVED_LEVELING_POINTS`
with an existing genuinely completed `HSTREAM_UI_E2E_GAME_ID`, a new artifact
directory, and a YAML file containing two pairs of reviewed original-image
coordinates (`[[[left_near_x, left_near_y], [left_far_x, left_far_y]],
[[right_near_x, right_near_y], [right_far_x, right_far_y]]]`). It opens the
ordinary Level rink dialog, places corners with mouse clicks, requires the native
rectangle estimate and rendered Preview, selects Use angles, and saves through
Save Preset. It then runs actual Calibration regeneration, captures Stitched,
stops normally, and reloads the saved controls. Use only independently reviewed
landmarks; the hook does not establish their physical accuracy. It rejects
`HSTREAM_UI_E2E_PREPARE_GAME` and must not be combined with fresh-import flags.
Pin `HM_CONFIG_ROOT` to the exact product configuration and include all tracked
configs when freezing a Bazel-shaped runtime; a test binary alone can otherwise
resolve installed configuration.

### Saved-map readiness and Play ownership

An optional saved-map readiness check can hold the calibration artifact lock even
when rink-aware framing is off. Cancellation does not release that ownership;
Play and Restart wait asynchronously for the worker to finish before admission.
Repeated Play cannot create another launch, and Stop, closing the window, a game
change or a newer pipeline generation cancels the pending request.

`rink_framing_controls_test` holds a real temporary artifact lock past cancellation
and checks that readiness retains ownership while the GUI continues processing
timers. The full `hstream_ui_test` uses the same lock with a fake runner to check
one deferred launch, repeated Play, Stop, game changes and generation fencing.
These CPU checks do not qualify actual GPU playback. The failed native scoreboard
startup remains retained; rerun it against the newly frozen runtime before
claiming that live case passes. The browser already uses supervised native
admission and retirement; no browser-side lock or readiness bypass is added.
