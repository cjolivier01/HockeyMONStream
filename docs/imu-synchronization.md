# Synchronizing recordings with camera motion

Hstream can synchronize GoPro and Insta360 recordings by correlating the
magnitude of their embedded gyroscope readings. This is the IMU mechanism used
by [video-stitcher](https://github.com/reco-project/video-stitcher/blob/master/crates/reco-calibrate/src/telemetry.rs).
It works when both cameras record the same rig movement, including handling or
vibration near the start of a recording.

Choose the method with a normal config override:

```bash
./run.sh --game-id=<game_id> --options=stitching.sync_method=imu -t=5
./run.sh --game-id=<game_id> --options=stitching.sync_method=auto -t=5
```

Or add this to a YAML overlay passed with `-c`:

```yaml
stitching:
  sync_method: auto
```

| Method | Behavior when calculating new offsets |
| --- | --- |
| `audio` (default) | Existing audio synchronization |
| `imu` | Require a reliable gyro match; report an error otherwise |
| `auto` | Try IMU, then log the reason and use audio if IMU is unavailable or unreliable |

Saved or manually supplied `game.stitching.frame_offsets` retain precedence.
Changing the method does not replace them. To recalculate an already configured
game, run `./run.sh --game-id=<game_id> --clean` (clears stitching calibration and
exits), then run with the desired method. `--clean-from-control-points` preserves
synchronization and therefore does not recalculate offsets. Both the one-pass
and `--two-stage` flows use the selected method.

The implementation reads metadata from the first physical chapter of each
camera playlist. It supports GoPro GPMF signed 16-bit gyro triples and Insta360
raw 20-byte or floating-point 56-byte gyro records, including their camera-clock
corrections. Other telemetry formats, stripped metadata, or absent gyro data
produce an IMU error (and audio fallback in `auto`). It does not derive gyro
signals from orientation quaternions.

The estimator resamples gyro magnitudes to 200 Hz, compares up to 30 seconds,
and searches offsets within ±5 seconds. It requires at least 100 samples per
camera, rejects timestamp gaps above 100 ms, and requires a correlation of at
least 0.7. Competing peaks within 0.05 of the best score and more than 100 ms
apart are rejected. Very still cameras, independent camera movement, repeated
motion, or larger start delays may require audio or manual offsets. Audio
fallback still requires usable audio tracks. The estimated offset is fixed;
this does not correct clock drift across long recordings.

Extraction runs only during synchronization setup. It demuxes compressed GoPro
packets or seeks directly into the Insta360 trailer, retaining at most 200,000
gyro samples per camera. GoPro demuxing has a 30-second wall-time limit. It opens
no video decoder, maps no video frames, and introduces no device-to-host copies
in the streaming pipeline. Frame skips use each camera's own frame rate.
