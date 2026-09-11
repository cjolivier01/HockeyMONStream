# HockeyMONStream — self-hosted multi-camera sports video

[![Latest release](https://img.shields.io/github/v/release/cjolivier01/HockeyMONStream?label=download)](https://github.com/cjolivier01/HockeyMONStream/releases/latest)
[![File-level licensing](https://img.shields.io/badge/licensing-mixed-blue.svg)](LICENSING.md)
[![C++](https://img.shields.io/badge/C%2B%2B-17-00599C.svg)](src)
[![NVIDIA DeepStream](https://img.shields.io/badge/NVIDIA-DeepStream-76B900.svg)](https://developer.nvidia.com/deepstream-sdk)

**HockeyMONStream is a free-to-download, source-available, self-hosted system
for stitching two camera videos into a panorama and automatically tracking the
play.** It turns fixed
wide-angle footage into a moving program view for recording or streaming youth
sports. The project began with ice hockey, but its video pipeline can be adapted
for other rink, court, and field sports.

[Product overview](https://cjolivier01.github.io/HockeyMONStream/) ·
[Download the latest release](https://github.com/cjolivier01/HockeyMONStream/releases/latest) ·
[Installation guide](https://cjolivier01.github.io/HockeyMONStream/install.html) ·
[Browse the source](src)

## What it does

1. Reads synchronized recordings from two cameras, including GoPro and
   Insta360 chapter layouts.
2. Calibrates overlap and stitches the views into one wide panorama on the GPU.
3. Detects and tracks the action, then pans and zooms a virtual camera to follow
   the play.
4. Adds optional scoreboard/graphics and writes an archive or routes video to
   live RTMP/RTSP outputs.

The implementation is a DeepStream-style C++17 application (`pipeline-app`)
with custom GStreamer, CUDA, and ONNX plugins for multi-camera synchronization,
video stitching, rink masking, object detection, play tracking, live routing,
and archive output. It is the performance-oriented counterpart to the Python
[HockeyMON](https://github.com/cjolivier01/HockeyMON) project. Production
calibration uses native C++/ONNX and does not invoke Python.

### Who it is for

- Youth and amateur sports teams that want control of their footage and compute.
- Developers building an automatic sports camera or multi-camera video pipeline.
- Researchers experimenting with sports video stitching, detection, tracking,
  virtual pan/tilt/zoom, or GPU video processing.

HockeyMONStream is an independent, source-available option for people evaluating
automated sports-camera products such as Pixellot, Hudl, or Veo. It is not a
drop-in clone and is not affiliated with those companies; their names and
trademarks belong to their respective owners. Unlike a hosted subscription,
HockeyMONStream provides an inspectable pipeline and runs on your NVIDIA GPU
hardware without a HockeyMONStream subscription fee. The repository has mixed
file-level licensing; see [Licensing](#licensing) before modifying or
redistributing it.

## Download

The [latest GitHub release](https://github.com/cjolivier01/HockeyMONStream/releases/latest)
contains installers/packages for these targets:

| Platform | Release asset | Notes |
| --- | --- | --- |
| Windows 11 | `*_windows-wsl-setup.exe` | Installs the app in a dedicated WSL 2 environment; requires an NVIDIA GPU and DeepStream package. |
| Ubuntu 24.04 / 26.04 x86_64 | `*_amd64.deb` | Use with the matching NVIDIA DeepStream installation. |
| NVIDIA Jetson (Ubuntu 22.04 arm64) | `*_jetson-ubuntu22.04_arm64.deb` | Built for JetPack 6-class Jetson systems. |

Release assets include `SHA256SUMS`. The Windows installer is currently signed
with a private/self-signed publisher certificate, so Windows reports an unknown
publisher unless that certificate is trusted. See the
[installation guide](https://cjolivier01.github.io/HockeyMONStream/install.html)
and [Windows WSL details](docs/windows-wsl-installer.md) before installing.

## Licensing

This repository is source-available but is **not covered by one blanket
open-source license**. It contains files under the root MIT license, Apache-2.0
components, and NVIDIA-derived files marked `LicenseRef-NvidiaProprietary`.
NVIDIA DeepStream and downloaded model assets also have their own terms. A
file's specific notice controls when present; the root MIT license does not
override it. Read [LICENSING.md](LICENSING.md) before use, modification, or
redistribution.

The repository and public product are named HockeyMONStream. Existing runtime
and packaging identifiers retain their legacy HStream names for compatibility:
the `hstream`, `hstream-cli`, `hstream-ui`, and `hstream-assets` executables;
the `hstream` Debian package; HStream UI and installer labels;
`~/.hstream/hstream.yaml` and `HM_*` configuration interfaces; and the
`/opt/hstream` install root.

This repo also includes DeepStream-Yolo-derived model conversion/config docs under `docs/` and DeepStream config snippets under `configs/deepstream/`.

## Build from source

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
   - Expected camera layout matches HockeyMON video discovery:
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

Launch the desktop UI with `make run-hstream-ui`, or run the built
`bazel-bin/src/apps/hstream-ui/hstream-ui` directly. On Linux, an uninstalled
build registers its HStream launcher and hockey icon under the user data
directory (`~/.local/share`, or `XDG_DATA_HOME`) before showing its window, so
the desktop taskbar can identify command-line launches. Existing custom or
package-installed desktop entries take precedence.

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
then command-line overrides. On first use HockeyMONStream creates the user overlay with:

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

The bundled baseline is an exact copy of HockeyMON's `hmlib/config/baseline.yaml`
at the revision pinned in `scripts/hmlib-runtime-revision` (currently
`cdaf03b13d23f5188e73dd938c3adf341070c972`). It is the default for source,
Bazel/runfiles, and `/opt/hstream` package runs. Native play-tracker settings
are materialized from the fully merged configuration, so lower-level plugin
files do not maintain a second set of baseline defaults.

Every baseline field with a native HockeyMONStream consumer is translated from that
same merged configuration. This includes stitching enable/blend/precision and
rotation, play-crop and plotting controls, tracker tuning, scoreboard geometry,
and archive bitrate/path/dimensions. Structural app YAML describes pipeline
topology and may provide an intentional native value; an explicit canonical
user, game, or CLI value overrides lower layers. At the same explicit layer, a
direct native property wins. Baseline fields for features that exist only in
HockeyMON/Aspen remain present in the merged YAML but have no invented HockeyMONStream
mapping until a native consumer exists.

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
