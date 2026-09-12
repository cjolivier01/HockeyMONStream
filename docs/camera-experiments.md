# Camera experiments

Camera experiments replay a short passage from its historical play-tracker state.
Change pan speed, acceleration, braking, zoom, or player filtering and compare the
result with the recorded camera. Every trial starts from the same state immediately
before the first selected sample, including living boxes, motion, and player history.

## Record the inputs

Enable **DriveGPT database** when recording. Each completed run publishes one
`hstream_telemetry-N.db` in the game directory. It contains detections, ordered
tracks, camera outputs, timestamps, configuration history, rink masks, exact
native inputs, and periodic checkpoints. No CSV companions or saved stitched MP4
are required. See [telemetry databases](telemetry-database.md) for the shared format.

Keep the **original camera chapters, game `config.yaml`, and stitching maps used
for the recording**. Experiment preview can decode and stitch those sources on
the GPU for just the selected passage, without saving a stitched movie or rerunning
inference. The configured camera order, chapter order and synchronization offsets
are reused. Existing maps must produce the recorded canvas; preparation does not
recalibrate them. Preserve historical maps if you later change the stitching.

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
3. Set **In recording** (seconds relative to the first telemetry sample) and
   **Duration** (normally 10–30 seconds). The end is exclusive.
4. Set **First selected frame in video** to the matching video's timestamp, in
   seconds. For original cameras, this is the position in the synchronized source
   timeline, before any recording-specific start offset. This is an explicit time
   binding: matching dimensions cannot establish that the sources contain the
   same footage or use the historical maps. Confirm the source and geometry
   checkbox after checking their identity and alignment.
5. Click **Prepare historical start**. Preparation queries the nearest preceding checkpoint and
   advances through the original inputs to the selected boundary. It verifies the
   recorded camera trajectory before making the session available.
6. Select the fast and/or Program/follower box, enable the parameters to override,
   give the trial a name, and click **Apply & replay**. Unchecked parameters retain
   their historical values. If **Video source confirmation** is unchecked, replay
   points to that checkbox before calculating a trial or advancing its number.
7. Use **View** to compare the recorded original, recomputed baseline, and named
   trials. **Repeat range**, the timeline, and the frame buttons revisit the same
   passage. A frame step completes when the GPU renderer presents its matching
   video frame. The camera plot compares the original and selected Program
   camera's horizontal position.
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
Program controls and presets.

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
capture, and backend tests also run on ARM64/Jetson.

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
  --confirmation /path/hstream_telemetry.db
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
