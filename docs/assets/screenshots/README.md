# Website UI screenshots

Captured from the running `hstream-ui` on 2026-09-09, using `vegas-kings-1`.
The source checkout at capture start was `06d72120`. The previews show second-period game
action around 25 minutes into the recording. The visible transport timestamps
identify the individual captures.

| Asset | UI view | Visible playback time |
| --- | --- | --- |
| `vegas-kings-program.webp` | Program preview with live tracking controls | 25:49 |
| `vegas-kings-stitched.webp` | Stitched panorama with controls collapsed | 25:37 |
| `vegas-kings-camera.webp` | Camera 1 source preview | 25:22 |
| `vegas-kings-mapping.webp` | Stitched preview with the Algorithms controls | 25:56 |

The game's saved calibration uses SuperPoint + LightGlue, NONA, GoPro Hero 11
camera geometry, and General Panini projection with parameters `100, -10, -10`.
The horizontal projection field of view is 185 degrees, with automatic canvas
sizing, saved leveling angles `[0, -24.675, -1.107]`, and a manual crop. The
resulting stitched canvas is 13875 × 3853; the Program output is 7680 × 4320.

To refresh these assets:

1. Make an isolated copy of the game's configuration and calibration artifacts,
   with symlinks to its camera recordings. Set `HM_GAME_DIR` to that copy's parent.
2. Launch `bazel-bin/src/apps/hstream-ui/hstream-ui` on an NVIDIA X11 display,
   load the game, enable Render video, and play in Program mode.
3. Seek to roughly 25 minutes, wait for the preview to resume, and choose frames
   showing active play. Inspect the footage rather than selecting on time alone.
4. Capture the Program, Stitched, and Camera 1 tabs. Expand Stitched Controls
   and select Algorithms for the mapping screenshot. The geometry controls are
   disabled during playback, as shown in the application.
5. Capture the application window at 1600 pixels wide. Crop off the bottom
   runtime log while retaining the transport, tabs, video, and preview status.
   Save WebP images at quality 88. Update the HTML dimensions if sizes change.

These are window captures of the real GPU previews. Image processing is limited
to cropping the runtime log and WebP compression. Check `docs/index.html` at
desktop and mobile widths, including every full-size image link, after updating.

## Camera experiments

`camera-experiment-original.webp` and `camera-experiment-slower-pan.webp` were
captured from the real Camera experiments dialog on 2026-09-10 on an NVIDIA
RTX 5090/X11 display. The input is the uncropped `sabercats-16a` stitched archive;
a fresh production detection/tracking run recorded the exact native replay
inputs and checkpoints from that video.

Both images show sample 1200, 9.994 seconds into a 20-second selected range
(video PTS 619.886 seconds). The trial overrides follower pan speed X to 1.5
and acceleration X to 0.2. The original camera's left edge is 15.9 pixels and
the trial's is 754.2 pixels at this same sample, demonstrating the effect of
slower panning from identical historical state and observations.

The opt-in `camera_experiment_dialog_test --e2e` exercises actual GPU playback,
paused matching-frame stepping, original/trial comparison, and looping. It
captures the presented GPU texture once and combines it with the actual Qt
dialog. Processing is limited to resizing to 1600 pixels wide and WebP quality
88 compression. No video frames or UI controls were fabricated. See the
[experiment workflow](../../camera-experiments.md) to reproduce with your own
recording and corresponding media time binding.
