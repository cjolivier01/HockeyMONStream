# Camera experiments

Camera experiments replay a short passage from its historical play-tracker state.
Change pan speed, acceleration, braking, zoom, or player filtering and compare the
result with the recorded camera. Every trial starts from the same state immediately
before the first selected sample, including living boxes, motion, and player history.

## Record the inputs

Enable **DriveGPT database** when recording. Each completed run publishes one
`<game-id>_telemetry-N.db` in the game directory. It contains detections, ordered
tracks, camera outputs, timestamps, configuration history, rink masks, exact
native inputs, and periodic checkpoints. No CSV companions or saved stitched MP4
are required. See [telemetry databases](telemetry-database.md) for the shared format.

Keep the **original camera chapters, game `config.yaml`, and stitching maps used
for the recording**. Experiment preview can decode and stitch those sources on
the GPU for just the selected passage, without saving a stitched movie or rerunning
inference. The configured camera order, chapter order and synchronization offsets
are reused. Existing maps must produce the recorded canvas; preparation does not
recalibrate them. Preserve historical maps if you later change the stitching.

Original-camera preview uses the effective seam blend and feather width from the
baseline, user settings, and selected game's configuration. Native stitcher aliases
take precedence within each layer. Preparation freezes the resolved settings, and
saved trials record them alongside the other stitching settings. Unsupported blend
modes fail preparation before the GPU graph starts.

An **uncropped Stitched archive** is also supported. It may be proportionally
downsized, with up to two pixels of encoder alignment rounding, provided it has no
padding or cropping. Camera coordinates remain in the recorded canvas and the
cropper scales them to the archive's resolution. A Program archive contains only
the old camera view and cannot reveal pixels outside that crop.

## Try a camera change

1. Click **Camera experiments** in the HStream window.
2. Select the completed **DriveGPT recording** database. For a merged database,
   select the game/run in **Recording**. Choose **Original cameras ·
   stitch during replay** and the original game directory, or **Saved uncropped
   panorama** and an existing archive.
3. Check **Playback start**, inherited from the main window and editable only in
   this dialog. Set **Start offset** relative to it and **Duration** (normally
   10–30 seconds). All time fields accept seconds (`12.5`), `MM:SS` (`90:23`
   means 90 minutes and 23 seconds), or `HH:MM:SS`, including milliseconds.
   Offsets must be zero or greater. The end is exclusive. Changing this dialog
   never writes Playback start back to the main app configuration.
4. **Video start offset** follows Start offset and can be corrected independently
   for differently aligned media. The archived recording's launch origin maps
   source time to telemetry time; older captures without that provenance assume
   their first sample corresponds to Playback start. Saved panoramas with an
   archive sidecar use its own source-time origin. Confirm the source and geometry
   checkbox after checking identity and alignment; matching dimensions alone
   cannot establish that sources contain the same footage.
5. Click **Prepare historical start**. Preparation queries the nearest preceding checkpoint and
   advances through the original inputs to the selected boundary. It verifies the
   recorded camera trajectory before making the session available.
6. The experiment starts with a snapshot of the main window's current camera
   settings, including player filtering and the selected fast/Program boxes.
   These parameters are enabled for the trial; uncheck any parameter to retain
   its historical value instead. Edit the settings, give the trial a name, and
   click **Apply & replay**. If **Video source confirmation** is unchecked, replay
   points to that checkbox before calculating a trial or advancing its number.
7. Use **View** to compare the recorded original, recomputed baseline, and named
   trials. **Repeat range**, the timeline, and the frame buttons revisit the same
   passage. A frame step completes when the GPU renderer presents its matching
   video frame. Click the **Visual timeline** to seek directly to a frame and
   inspect it paused, then press **Play preview** to continue from there. The
   timeline compares the original and selected Program camera's horizontal position.
8. **Save trial** writes a YAML descriptor with the starting checkpoint, selected
   inputs, overrides, camera trajectory, and media/time binding. Original-camera
   bindings include the prepared config, chapter identities, synchronization
   offsets and stitching revision. **Screenshot**
   captures the displayed frame and experiment controls.

Original means the camera trajectory in the CSV. Recomputed baseline means the
historical starting configuration held constant across this passage. A candidate
holds that configuration plus its overrides. Recorded control changes after the
selected start therefore remain visible in Original, but do not overwrite a trial.
Experiments use their own controls and tracker instances, independent of live
Program controls and presets. Changes in an open experiment do not change the
main window, and later main-window changes do not overwrite the experiment.

## Rink-aware framing experiment

With **Original cameras** selected, prepare a recording whose calibration matches
a saved, reviewed rink map. The **Rink framing** tab enables an optional framing
adjustment with independent controls for anticipating play, including the nearby
goal, and widening during rushes or tightening during settled play. It defaults
off. Partial, missing or stale maps leave this option unavailable with a reason;
ordinary camera experiments remain available.

Apply creates two named views, **native** and **rink-aware**, with identical
camera overrides. They differ only in the rink adjustment and replay exactly
the same native player observations and starting state. Switching views while
paused keeps the inspected sample for direct A/B comparison. The
guide estimates collective motion from box feet in rink metres; it has no puck
or home/away classifier. Existing ice-mask filtering and native player exclusions
are preserved. The timeline shows each resulting Program path and crop width;
the frame label reports speed, added lead, width ratio and goal-framing requests.
A goal request is a framing preference, not a guarantee that the goal fits at
every instant while the camera catches up.

The adjustment uses at most 256 observations and 512 motion histories, rejects
implausible motion jumps, waits for three consistent tracks, and resets across
time gaps and seeks. Pan/zoom changes are bounded and smoothed. Without reliable
motion it returns toward the native camera. This is an experiment-only option:
live Program tracking, telemetry measurements and game presets are unchanged.
Save trial retains the exact map evidence, settings, calibration identity and
per-frame diagnostics alongside its camera trajectory.
The **Play tracking** overlay continues to show the native policy's boxes and
movement thresholds before the adjustment; the visual timeline and frame label
show the selected trial's final Program rectangle.

`RinkCameraGuide.*` performs this metadata-only computation inside trial replay.
`PlaybackRinkMap::Project` prepares the two goal anchors with a bounded inverse
projection; it does not run for every player/frame. Original-camera preparation
retains the published maps' width cap, including capped historical canvases.

For a numerical comparison without video decoding:

```sh
bazelisk build --config=opt //src/libs/playtracker_replay:rink_camera_comparison
bazel-bin/src/libs/playtracker_replay/rink_camera_comparison \
  /path/game_telemetry-1.db /path/game 600 20 /tmp/comparison.csv
```

Offsets are relative to the recording's first PTS. The output aligns baseline
and trial rectangles to the same sample and reports guide diagnostics. Existing
output files are refused. For matched GPU screenshots and a moving A/B playback
check, the dialog test accepts `--rink-comparison RECORDING.db GAME_DIRECTORY
ARTIFACT_DIR OFFSET DURATION PLAYBACK_START`; the final value is the original
source-time launch origin in seconds. This check captures three matching samples
from both views; images stay outside the source repository.

## Preview controls and drawings

**Player boxes**, **Play tracking**, and **Rink mask** above the preview can change
while playing or paused. Player boxes use the recorded observations, with stable
colors and gray `IGNORED` labels for the selected camera policy's exclusions.
Play tracking uses the same camera boxes, thresholds, clusters, and movement
geometry as the live preview, computed for Original, Baseline, or the selected
trial. These drawing choices stay in this dialog and do not change camera policy.

The rink mask comes from the selected database's archived geometry, not the
current game's calibration. Missing, invalid, or unsupported archived masks disable
that toggle with an explanation. Legacy CSV recordings can draw players and
tracking but do not contain a historical rink mask. Scaled panoramas and camera
rotation use the production cropper's coordinate transform for every layer.
Changing drawings on a paused frame reuses one retained GPU output buffer; it does
not seek, rerun inference, or reconstruct the source graph.
Pausing keeps the image visible. Resizing or maximizing a paused preview redraws
that same retained GPU frame without advancing playback.

Preparation and preview startup, seeks, loops, and teardown run in the background.
**Play at normal speed** is checked by default, preserving timestamp-paced playback.
Uncheck it to process the preview as fast as possible. It is grayed out while
playing or loading; pause or stop first, then press **Play preview** to apply the
choice. Both modes retain late video frames, and the setting does not change trial
trajectories or saved recordings. Camera Experiments has no audio playback branch.

An animated progress bar identifies pending work. Source and camera settings are
disabled during preparation and playback; pause the preview to edit them. The dialog can resize and maximize, and the
icon at the Program preview's top right expands or restores the preview layout. **Stop** closes the preview and
restores the normal layout. Cancel stops pending work, and closing the dialog keeps the window responsive while the
renderer finishes using its native video window. Cancellation can take time while
a decoder operation finishes.

Changing recording, range, or media invalidates the prepared session. A range must
stay within one source, seek/reset epoch, and canvas/arena geometry. Preparation
reports missing or inconsistent artifacts instead of substituting default state.

## Older CSV recordings

The **Legacy recording** controls accept the historical arena as
`left, top, right, bottom` in recorded canvas pixels, and the original policy file
if its archived copy is unavailable. Reconstruction replays the recorded player
tracks and configuration events from the actual initialization boundary. Both
camera trajectories must match the recording within the stated tolerance.

This mode is labeled trajectory-verified reconstruction. It cannot promise an
exact historical internal state from camera boxes alone, and refuses recordings
whose missing inputs or mismatched trajectory prevent verification. Do not supply
today's arena or policy unless it is also the one used for that recording.

## Implementation and validation

`src/libs/playtracker_replay` provides CPU preparation and trial computation using
the same native tracker and runtime tuning code as production. The native snapshot
schema is versioned and bounded. Compatibility is checked before restoring a fresh
tracker; deterministic continuation is tested on the same build/platform.

The Qt preview reuses GPU decoding, the production playcropper, and the existing
GPU renderer. Original-camera replay also reuses production chapter selection,
exact source pairing and GPU stitching. Raw seeks and loops rebuild this small
source graph on a worker; chapter positioning seeks to a nearby keyframe and trims
to the selected time. Maps are validated again against the prepared revision at
load, so changes require preparation again. Archive loops reuse their decoder
graph. Video stays GPU-resident during playback; an explicit screenshot performs one bounded readback of the displayed
preview. Qt preview requires the existing x86_64/X11 GPU environment. Native replay,
capture, and backend tests also run on ARM64/Jetson. Experiment preparation opts
into bounded, immutable drawing snapshots through the shared native drawing
routine. Ordinary playback seek preparation leaves this work disabled. Archived
mask PNGs use the existing bounded decoder once during background preparation;
this is static recording data, not video readback.

Relevant test targets:

```sh
bazelisk test --config=opt --cpu=k8 \
  //src/libs/playtracker_replay:playtracker_replay_test \
  //src/gst-plugins/gst-videoprep/playtracker:playtracker_telemetry_csv_test \
  //src/apps/hstream-ui:telemetry_csv_publisher_test \
  //src/apps/hstream-ui:camera_experiment_dialog_test
```

The source-confirmation regression check accepts a completed recording and runs
without video playback or an X11 display:

```sh
QT_QPA_PLATFORM=offscreen bazel-bin/src/apps/hstream-ui/camera_experiment_dialog_test \
  --confirmation /path/my-game_telemetry-1.db
```

The dialog test also has an opt-in real GPU exercise. Supply a completed recording,
the corresponding game directory or panorama, output directory, recording offset, duration, and the
first selected video's timestamp:

```sh
QT_QPA_PLATFORM=xcb bazel-bin/src/apps/hstream-ui/camera_experiment_dialog_test \
  --e2e /path/hstream_telemetry.json /path/panorama.mp4 /tmp/experiment-ui \
  10 20 609.892
```

The example timestamp is recording-specific; determine the correct mapping for
your own media. See [telemetry format](playtracker-telemetry-csv.md) and the
[accepted design](camera-experiments-design.md) for persistence and replay details.

## Source throughput comparison

The manual `//src/apps/hstream-ui:camera_experiment_source_benchmark` target compares
GPU decode/stitch with a bounded FFV1 CPU cache. Run it separately from preview:

```sh
camera_experiment_source_benchmark GAME WIDTH HEIGHT START_SECONDS FRAMES /tmp/clip.mkv direct
camera_experiment_source_benchmark GAME WIDTH HEIGHT START_SECONDS FRAMES /tmp/clip.mkv cache
camera_experiment_source_benchmark GAME WIDTH HEIGHT START_SECONDS FRAMES /tmp/clip.mkv decode
```

The cache comparison deliberately downloads full stitched frames to CPU I420 for
FFV1 encoding; decoding uploads them back to the GPU. This is an explicit bounded
benchmark, not a preview path. The frame count must be positive. Throughput excludes
crop/render; total time includes graph startup and shutdown.

On the local RTX 5090, two moving 7680×4320 HEVC Main10 camera sources produced a
6626×2467 canvas at about 188 fps over 120 output frames. Seeking to ten minutes and
processing those frames took 2.48 seconds overall. A 120-frame FFV1 cache encoded
at about 36 fps with automatic threading (5.85 seconds overall), and software
playback including GPU upload measured about 53 fps on the same passage. These
short measurements favor direct replay on this machine; they do not establish
throughput for other codecs, canvas sizes, hardware, or simultaneous live pipelines.
