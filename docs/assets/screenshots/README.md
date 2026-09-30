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
| `vegas-kings-mapping.webp` | Stitched preview with the legacy Algorithms controls | 25:56 |

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
   and select Projection for the mapping screenshot (the existing image predates the Alignment/Projection split).
   The geometry controls are disabled during playback, as shown in the application.
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

## README assets

Assembled on 2026-09-30 from source checkout `0d55128c` on an NVIDIA RTX 5090.
These four files are embedded in the repository [README](../../../README.md).
Every pixel of video in them is real pipeline output. Processing was limited to
trimming, scaling, compositing onto a solid background, adding text labels, and
WebP compression. No frame was retouched, and no detection box, overlay, or UI
control was drawn or fabricated.

| Asset | Dimensions | Size | Kind |
| --- | --- | --- | --- |
| `auto-follow-cam.webp` | 960 × 540 | 2.4 MB | Animated, 98 frames at 12 fps, infinite loop |
| `whole-rink-live.webp` | 1200 × 306 | 0.8 MB | Animated, 84 frames at 12 fps, infinite loop |
| `two-cameras-one-panorama.webp` | 1600 × 926 | 203 KB | Still composite |
| `four-rinks.webp` | 1600 × 962 | 176 KB | Still composite |

Animation is WebP rather than GIF on purpose. The same eight-second follow-cam
clip is 2.4 MB as animated WebP and 25 MB as a 256-colour GIF, because a
full-frame pan over ice texture defeats GIF interframe compression. GitHub
serves committed `.webp` as `image/webp`, and a browser without animated-WebP
support falls back to the first frame rather than failing.

All four show the burned-in "SportsAI HockeyMON" watermark described in
[the watermark notes](../../watermark.md). It cannot be disabled.

### `auto-follow-cam.webp`

Source: `vegas-kings-1-tracking_output-with-audio-1.mp4`, the finished
7680 × 4320 HEVC Program render this pipeline produced for `vegas-kings-1` on
2026-09-09. The excerpt starts 2458.6 s into that 1:03:53 render and runs 8.2 s.
The visible scoreboard goes from 1:38 to 1:30 in the second period, over a rush
that carries the virtual camera from one end zone, through center ice, to the
attacking end. The clip is continuous and unedited; the camera motion is the
pipeline's, not a post-production pan.

```
ffmpeg -ss 2458.6 -i vegas-kings-1-tracking_output-with-audio-1.mp4 -t 8.2 \
  -vf "fps=12,scale=960:-1:flags=lanczos" -c:v libx264 -crf 10 -an hero_src.mp4
ffmpeg -i hero_src.mp4 -c:v libwebp_anim -q:v 62 -compression_level 6 -loop 0 \
  auto-follow-cam.webp
```

### `whole-rink-live.webp`

A live stitched panorama, captured fresh on 2026-09-30 rather than reused from
an existing render. `hstream-cli` ran against an isolated sandbox copy of
`vegas-kings-1` (configuration and calibration artifacts copied, camera
recordings symlinked, `HM_GAME_DIR` pointed at the copy), with detection,
tracking, play tracking, cropping, and audio disabled so only the stitcher and
the stitched-archive sink were active.

```
HM_GAME_DIR=<sandbox parent> ./run.sh --game-id=vegas-kings-1 \
  --enable-sinks=ENCODE_STITCHED_FILE \
  --options=stitching.control_point_resolution=native \
  --options=pipeline.sink5.output-file=<abs path>/stitched.mkv \
  --options=pipeline.sink5.codec=2 \
  --options=pipeline.primary-gie.enable=0,pipeline.ds-playtracker.enable=0,\
pipeline.hmplaycropper.enable=0,pipeline.tracker.enable=0,pipeline.hmaudio.enable=0 \
  --start-time=00:40:55 -t=10
```

`control_point_resolution=native` is required: `configs/baseline.yaml` defaults
to `2k`, and this game's saved artifacts were built at native resolution, so the
default aborts the run with a superseded calibration generation.

That run recalibrated stitching in process and produced a 13485 × 3442 canvas,
which the archive encoder scaled to 8192 × 2090 for HEVC. The animation is 7 s
starting 1.5 s into the 10 s clip, at `fps=12,scale=1200:-2`, encoded with
`libwebp_anim -q:v 62`. This is the **uncropped** stitched canvas, so the dark
wedges in the upper corners are real canvas edges, not processing artifacts;
they are what the per-game crop setting exists to remove.

### `two-cameras-one-panorama.webp`

Composited from three artifacts written by this game's stitching calibration on
2026-09-12: `left.png` and `right.png` (each 7680 × 4320, the two projected
camera views) above `panorama.tif` (13891 × 3388, the stitched result). The same
center-ice logo and painted "GHOST" wordmark appear in both camera views, which
is the overlap the calibration solves. Camera views were scaled to 788 × 444 and
the panorama to 1600 × 390 on a solid `#0B1220` canvas, with DejaVu Sans Bold
labels, then saved at WebP quality 88. The three images are unmodified apart
from that scaling.

### `four-rinks.webp`

One frame from each of four Program renders, showing four venues, lighting
setups, and camera placements handled by the same build with per-game
calibration:

| Tile | Game | Source render | Frame time |
| --- | --- | --- | --- |
| Top left | `vegas-kings-1` | 7680 × 4320 tracking output | 1020 s |
| Top right | `sharks-14-p1` | 3840 × 2160 4K Program output | 2100 s |
| Bottom left | `blackhawks-p1` | 3840 × 2160 4K Program output | 1800 s |
| Bottom right | `tv-14-1-p1` | 3840 × 2160 4K Program output | 3600 s |

Frames were chosen by inspecting contact sheets sampled across each render and
picking live play; several evenly spaced timestamps landed on stoppages and
bench huddles and were rejected. Each frame was scaled to 794 × 446 and placed
in a 2 × 2 grid on a solid `#0B1220` canvas at WebP quality 88.

### Not captured

A detection-and-tracking overlay (player boxes, play-tracking camera box, rink
mask) is **not** part of this set. Three routes were tried on 2026-09-30 and
none produced a publishable, correctly aligned image:

- **GPU preview overlays** (`HSTREAM_UI_E2E_PREVIEW_OVERLAYS=players,play,rink`
  with `scripts/test_hstream_ui_e2e.sh --x11-preview`) need a real X server. This
  host runs Xwayland, where the embedded GPU preview never delivered a first
  frame; the run sat at 0.00 FPS through four recovery attempts. This matches the
  warning in [the E2E notes](../../hstream-ui-e2e-testing.md) that Xwayland is
  unreliable for framebuffer grabs.
- **Burning boxes into encoded output**
  (`pipeline.hmplaycropper.plot-player-tracking=1`) needs the Program branch,
  which never produced frames in this headless configuration and hung in EOS
  finalization, leaving a zero-byte file. The stitched-archive branch ran fine in
  the same environment, so this is specific to the Program path here.
- **Drawing recorded telemetry onto an existing render** was rejected as
  incorrect. `hstream_telemetry-2.db` holds a complete 229,781-sample run with
  per-frame detections, tracks, and Program camera rectangles in
  `original_stitched_pixels` on a 13891 × 3388 canvas, but that run is from
  2026-09-12 while the 8K render is from 2026-09-09, with a recalibration in
  between. Overlaying the two puts the boxes visibly off the players.

Confirmed while testing: `pipeline.hmplaycropper.plot-play-tracking` and
`pipeline.ds-playtracker.draw` affect the display/render copy only and do **not**
appear in the encoded stitched archive.
