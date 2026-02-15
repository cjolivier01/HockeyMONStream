# hstream

High-performance hockey video processing pipeline built on NVIDIA DeepStream + GStreamer.

This repo contains a DeepStream-style C++ app (`pipeline-app`) plus custom GStreamer plugins for dual-camera stitching, rink masking, and play tracking. It is the performance-oriented counterpart to the Python implementation in the sibling `hm` repo and reuses HockeyMOM configs (and optionally its Python CLI helpers) for configuration steps.

This repo also includes DeepStream-Yolo-derived model conversion/config docs under `docs/` and DeepStream config snippets under `configs/deepstream/`.

## Quick Start

1. Install system deps:
   - DeepStream installed under `/opt/nvidia/deepstream/deepstream` (see `WORKSPACE.bazel`).
   - CUDA/TensorRT/OpenCV headers available.
   - Some DeepStream components have extra runtime deps (e.g. tracker needs `libmosquitto1`).
   - Bazelisk (`scripts/install_bazelisk.sh` or let `./bld` prompt).

2. Build:
   - `./perf` (release) or `./bld` (debug)

3. Prepare a game directory:
   - Default: `$HOME/Videos/<game_id>`
   - Override: set `HM_GAME_DIR=/path/to/games_root` and it will use `${HM_GAME_DIR}/<game_id>`
   - Expected camera layout matches HockeyMOM video discovery:
     - `${game_dir}/cam1/*.mp4`
     - `${game_dir}/cam2/*.mp4`
     - (GoPro and Insta360 chapter naming patterns supported)

4. Run end-to-end:
   - `./run.sh --game-id=<game_id> -t=5`

`run.sh` performs a two-stage run:
- Stage `-1`: stitching + rink mask configuration (runs with a `FAKE` sink)
- Stage `0`: main pipeline (defaults to `RENDER` sink unless you pass `--enable-sinks=...`)

## Models / Pretrained Assets

The default `configs/config_infer_yolox_hockey.yaml` expects YOLOX-s COCO assets under `pretrained/deepstream/yolox/`.

- `run.sh` will automatically invoke `scripts/setup_yolox_s_pretrained.sh` if the default weights/labels/ONNX are missing.
- Model artifacts are not committed. The setup script downloads YOLOX weights + COCO labels and exports an ONNX model via `utils/export_yolox.py`.
- `pretrained/` is often a symlink to a large mounted volume; the script can `sudo`-create/chown the needed subdirectory when required.

## Configuration

Runtime behavior is controlled by YAML configs in `configs/` and CLI overrides:
- Primary configs: `configs/ds_hockey_configure_stitching.yaml`, `configs/ds_hockey_app_config.yaml`
- Inference config: `configs/config_infer_yolox_hockey.yaml`
- Play tracking defaults: `configs/play_tracker_config.yaml`

HockeyMOM baseline config root:
- Auto-detected from a sibling `../hm/hmlib/config` checkout when present, or from Bazel's `hm` external repo after a build.
- Override explicitly via `HM_CONFIG_ROOT=/path/to/hm/hmlib/config`

## Repo Layout

- `src/apps/pipeline-app`: main DeepStream app
- `src/gst-plugins`: custom plugins (videoprep/stitcher/playtracker/fieldmask/etc)
- `src/libs/*`: C++ libraries (stitching, overlays, scoreboard, camera utilities, etc)
- `configs/`: YAML configs
- `scripts/`: helper scripts (e.g. pretrained setup)

## Pointers

- Jetson cross-build docs: `docs/jetson-cross-build.md`
- Dual IMX477 recorder app: `src/apps/dual-record/README.md`

## Acknowledgements

This repo contains code and build patterns derived from the DeepStream-Yolo ecosystem (see `LICENSE.md` and `docs/`).
