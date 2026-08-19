# hstream

High-performance hockey video processing pipeline built on NVIDIA DeepStream + GStreamer.

This repo contains a DeepStream-style C++ app (`pipeline-app`) plus custom GStreamer plugins for dual-camera stitching, rink masking, and play tracking. It is the performance-oriented counterpart to the Python implementation in the sibling `hm` repo. Production calibration is native C++/ONNX; Python is used only by an explicit, source-checkout parity test when HockeyMOM is installed.

This repo also includes DeepStream-Yolo-derived model conversion/config docs under `docs/` and DeepStream config snippets under `configs/deepstream/`.

## Quick Start

1. Install system deps:
   - DeepStream installed under `/opt/nvidia/deepstream/deepstream` (see `WORKSPACE.bazel`).
   - CUDA/TensorRT/OpenCV headers available.
   - Some DeepStream components have extra runtime deps (e.g. tracker needs `libmosquitto1`).
   - Bazelisk (`scripts/install_bazelisk.sh` or let `./bld` prompt).

2. Build:
   - `./perf` (release) or `./bld` (debug)
   - `make arm64` for native non-Jetson ARM64/SBSA hosts (for example GB300)
   - `make jetson` for Jetson targets/cross-builds

3. Prepare a game directory:
   - Default: `$HOME/Videos/<game_id>`
   - Override: set `HM_GAME_DIR=/path/to/games_root` and it will use `${HM_GAME_DIR}/<game_id>`
   - Expected camera layout matches HockeyMOM video discovery:
     - `${game_dir}/cam1/*.mp4`
     - `${game_dir}/cam2/*.mp4`
     - (GoPro and Insta360 chapter naming patterns supported)

4. Run end-to-end:
   - `./run.sh --game-id=<game_id> -t=5`
   - `scripts/test_hstream_ui_e2e.sh <game_id>` to drive the UI controls, exercise the scoreboard selector, archive output, and visually compare it with `panorama.tif`

`run.sh` performs a one-pass stage `0` run by default. If stitching artifacts
are absent, native calibration completes in-process before the stitcher output
pool is allocated. Pass `--two-stage` to run the older stage `-1` FAKE-sink
configuration followed by stage `0`.

The UI acceptance test runs against an isolated symlink sandbox, leaving the
source game untouched. It retains the complete pipeline log, UI screenshots,
an encoded sample frame, a panorama preview, and feature-match diagnostics
under `test-artifacts/`. See `docs/hstream-ui-e2e-testing.md`.

## Models / Pretrained Assets

The default `configs/config_infer_yolox_hockey.yaml` declares the YOLOX-s COCO assets it needs under
`pretrained/deepstream/yolox/`.

- `run.sh` scans the configured YAML files and downloads missing `pretrained-assets` before starting the pipeline.
- Model artifacts are not committed. The default config downloads the YOLOX-s ONNX model and COCO labels on demand.
- `pretrained/` is often a symlink to a large mounted volume; the setup helper can `sudo`-create/chown the needed subdirectory when required.

## Configuration

Runtime behavior is controlled by YAML configs in `configs/` and CLI overrides:
- Canonical defaults: `configs/baseline.yaml`
- Primary configs: `configs/ds_hockey_configure_stitching.yaml`, `configs/ds_hockey_app_config.yaml`
- Inference config: `configs/config_infer_yolox_hockey.yaml`
- Play tracking structure: `configs/play_tracker_config.yaml`

Configuration layers are applied in this order: the bundled `configs/baseline.yaml`, the
per-user `~/.hstream/hstream.yaml` overlay, the game's private `config.yaml`,
then command-line overrides. On first use HStream creates the user overlay with:

```yaml
paths:
  output-root: /home/you/hstream_output
```

Add `paths.game-root` there to replace the default `$HOME/Videos` game root.
`paths.output-root` replaces the old working-directory-dependent
`output_workdirs` behavior. `HM_GAME_DIR` and `HM_OUTPUT_WORK_DIR` remain
explicit environment overrides for automation.

Successful UI archive runs are losslessly remuxed (not re-encoded) into the
game directory as `<game-id>-tracking_output-with-audio.mp4`. The final MP4 is
published only after ffmpeg completes its fast-start compatibility pass.

The bundled baseline is an exact copy of HockeyMOM's `hmlib/config/baseline.yaml`
at commit `242f45c148cc5fbc6a6c7255e9f65b2034d75a4c`. It is the default for source,
Bazel/runfiles, and `/opt/hstream` package runs. Native play-tracker settings
are materialized from the fully merged configuration, so lower-level plugin
files do not maintain a second set of baseline defaults.

`HM_CONFIG_ROOT=/path/to/config` is an explicit diagnostic/development
override. If it is set but does not contain a valid `baseline.yaml`, startup
fails instead of silently selecting another copy.

## Repo Layout

- `src/apps/pipeline-app`: main DeepStream app
- `src/gst-plugins`: custom plugins (videoprep/stitcher/playtracker/fieldmask/etc)
- `src/libs/*`: C++ libraries (stitching, overlays, scoreboard, camera utilities, etc)
- `configs/`: YAML configs
- `scripts/`: helper scripts (e.g. pretrained asset setup)

## Pointers

- Jetson cross-build docs: `docs/jetson-cross-build.md`
- Dual IMX477 recorder app: `src/apps/dual-record/README.md`

## Acknowledgements

This repo contains code and build patterns derived from the DeepStream-Yolo ecosystem (see `LICENSE.md` and `docs/`).
