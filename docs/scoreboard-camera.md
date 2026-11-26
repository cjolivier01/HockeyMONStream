# Independent scoreboard camera

Use the camera icon (**Extra camera…**) beside the Program scoreboard controls to choose a separate
camera and its four crop corners. The desktop and browser dialogs keep source
coordinates even when the preview is scaled down. Position, scale, flips and
visibility continue to use the existing scoreboard controls.

For recordings, select original H.264/HEVC MP4 chapters in playback order. The
browser uses uploaded recordings that are unselected from the game cameras.
Desktop automatic camera discovery excludes the configured scoreboard chapters,
including recordings in a `camN` directory. The camera does not join the stitcher,
audio synchronization, tracking or panorama geometry. Do not assign its recordings
to an explicit game-camera role as well.

The start offset has the form `HH:MM:SS.ssss`, optionally signed. At output time
`t`, the scoreboard recording is sampled at `t + start_offset`:

- `00:00:05.0000` skips the scoreboard recording's first five seconds.
- `-00:00:05.0000` leaves the first five seconds of output without an overlay,
  then starts at scoreboard source time zero.
- Preview time selects a still in the scoreboard recording independently of the
  offset. It does not change playback timing.

The final compositor waits for the image whose presentation interval contains the
requested timestamp. Bounded decode-ahead applies backpressure. Seeks cancel the
old session and begin a new epoch; retained GPU images outlive their decoder
surfaces. Missing frames, unexpected timestamp gaps, early scoreboard EOS and
operation timeouts fail the run rather than substituting stale pixels. The
intentional negative-offset delay is the only interval without an overlay.
Fractional-rate timestamps use integer nanoseconds; one-nanosecond differences
from independent timestamp/duration rounding join at the next actual source PTS.

All shipped settings live under `rink.scoreboard.camera` in `configs/baseline.yaml`.
A recorded override looks like:

```yaml
rink:
  scoreboard:
    camera:
      enabled: true
      files: [cam3/scoreboard.mp4]
      source_dimensions: [1920, 1080]
      polygon: [[200, 100], [999, 100], [999, 399], [200, 399]]
      start_offset: "-00:00:05.0000"
```

A live source uses `uri` and `codec` instead of `files`. It requires live game
inputs. The NVIDIA game and scoreboard graphs share the system clock; explicit
offsets correct relative phase. Receiver-clock timestamps alone do not establish
sensor capture synchronization. Enable sender NTP synchronization only when the
cameras and game-source configuration provide a common synchronized timestamp
reference. The AMD Program runner and browser Program admission currently support
recorded game inputs; their existing live-game limitation remains. The shared VA
scoreboard decoder itself has loopback RTSP coverage.

Independent scoreboard playback requires the native HStream backend. The
HockeyMON Python Program backend rejects an enabled extra camera at launch;
it does not silently omit the overlay. Saved settings can still be edited or
disabled before switching backends. Browser HockeyMON Program admission remains
closed under its existing workflow qualification gate.

## Decode and GPU work

The NVIDIA and AMD paths decode the original stream in hardware. There is no
pre-transcoding or scoreboard-video cache. NVIDIA crops during GPU conversion;
AMD imports the VA surface and converts only its crop to a compact GPU image.
The bounded compact pool feeds the existing perspective warp. No video pixels
are read back to the CPU during playback.

These decoder paths do not offer arbitrary inter-frame ROI-only decoding.
[NVDEC's crop rectangle](https://docs.nvidia.com/video-technologies/video-codec-sdk/13.1/nvdec-video-decoder-api-prog-guide/index.html)
is output processing, not a way to omit decoding the rest of the compressed
picture. AMD's [rocDecode sample crop option](https://rocm.docs.amd.com/projects/rocDecode/en/latest/how-to/using-rocDecode-videodecode-sample.html)
also does not establish ROI-only decoding for the VA interop path. Full decode
cost therefore remains, especially for an additional 8K recording. HDR PQ/HLG
scoreboard inputs are rejected until a matching tone-mapping path exists.

**Load frame** is a separate, cancellable, CPU still request, bounded by the
baseline's source-pixel, preview-dimension and timeout limits. This happens only
on request while configuring the camera. The browser launches it through the
supervised native worker and validates the pinned source and image receipt before
allowing a new crop to be saved.

## Regression coverage and measurement

`camera_frames_test` covers interval boundaries, backpressure, gaps, cancellation,
EOS and epochs. `camera_decoder_test` checks H.264/HEVC, full-frame versus GPU-crop
pixels, retained images, partial-read shutdown, repeated seeks, chapters,
60000/1001 timestamps and RTSP. `scoreboard_camera_test` exercises the actual
NVIDIA stitcher and cropper with positive/negative offsets and seeks.
`program:gpu_test` covers the independent overlay in AMD/NVIDIA output kernels.
Desktop and browser interaction tests cover source-coordinate crops, offset
precision, maximize/restore, cancellation and wheel guards. The optional browser
native-preview test exercises admission through native receipt verification.

The stitch/crop benchmark accepts:

```sh
bazel-bin/src/gst-plugins/gst-videoprep/playcropper/scoreboard_camera_test \
  configs/baseline.yaml --benchmark MAIN_MP4 [SCOREBOARD_MP4]
```

It runs two main hardware decoders, the real stitcher and output cropper, with
20 warm-up and 100 measured frames per variant. It checks frame counts and PTS.
It excludes inference and encoding, so its result is not an end-to-end FPS claim.
One RTX 5090 run with two 8K inputs measured 110.4 FPS without the scoreboard and
110.2 FPS with a 1080p scoreboard. A third 8K recording measured 110.7 versus
105.6 FPS. These are individual measurements, not zero-regression guarantees.
