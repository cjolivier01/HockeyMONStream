# Repository Guidelines

## Project Structure & Modules
- Code: `src/` with libraries in `src/libs/*`, apps in `src/apps`, optional plugins in `src/gst-plugins`.
- Build files: `BUILD.bazel` next to sources; external repos in `WORKSPACE.bazel` and `buildfiles/`.
- Configs and assets: `configs/`, `env/`, `scripts/`, `docs/`.
- Tests: colocated with libraries (e.g., `ScoreboardTest.cpp`) and built as Bazel targets.

## Build, Test, and Run
- Debug build (all): `./bld` or `bazelisk build --config=debug //...`.
- Release build (all): `./perf` or `bazelisk build --config=opt //...`.
- x86_64 build: `bazelisk build --config=opt --cpu=k8 //...`.
- Jetson (aarch64) build: `bazelisk build --config=jetson //...`.
- Canonical runs:
  - End-to-end wrapper (recommended): `./run.sh --game-id=<game_id> -t=5`
    - Runs stage `-1` (stitching + rink mask configuration) with a `FAKE` sink, then stage `0` (main pipeline) with default `RENDER` sink.
    - Supports `-t N`, `-t=N`, or `--time-limit=N`.
    - If the default YOLOX assets are missing, it will invoke `scripts/setup_yolox_s_pretrained.sh`.
  - Direct `pipeline-app` invocation (useful for debugging configs):
    - Display only: `bazel-bin/src/apps/pipeline-app/pipeline-app -c configs/ds_hockey_configure_stitching.yaml -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=RENDER --options=pipeline.hmaudio.enable=1`
    - Encode to file: `bazel-bin/src/apps/pipeline-app/pipeline-app -c configs/ds_hockey_configure_stitching.yaml -c configs/ds_hockey_app_config.yaml --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1`
    - Both commands support an optional time limit: append `-t N` (or `--time-limit=N`) to stop after processing `N` seconds of video.
- Run a test binary (pattern):
  - `bazelisk run //src/libs/scoreboard:scoreboard_test`

Notes:
- This repo assumes CUDA/DeepStream/OpenCV and other system headers are installed; see `WORKSPACE.bazel` for local_repository paths (e.g., DeepStream under `/opt/nvidia/deepstream/deepstream`).
- Some DeepStream components have extra runtime shared-lib deps (e.g. tracker needs `libmosquitto1`).
- Game directories default to `$HOME/Videos/<game_id>`. Override with `HM_GAME_DIR=/path/to/games_root` (game dir becomes `${HM_GAME_DIR}/<game_id>`).
- HockeyMOM baseline config is auto-detected from a sibling `../hm` checkout when present; override explicitly via `HM_CONFIG_ROOT=/path/to/hm/hmlib/config`.
- Install Bazelisk once via `scripts/install_bazelisk.sh` (the `bld` script will prompt if missing).

## Coding Style & Naming
- Language: C++17 (see `.bazelrc`); format with `.clang-format`.
- Indentation: 2 spaces, no tabs; line length ≈ 120; left-aligned pointers; sorted includes.
- Bazel: use `cc_library` for reusable code and `cc_binary` for tools/tests. Prefer `INCLUDE_PREFIX` so includes look like `#include "hstream/src/libs/<mod>/Header.h"`.
- File names: `PascalCase` for classes/headers, `snake_case` for Bazel targets when consistent with neighbors.

## Testing Guidelines
- Keep tests small and colocated. Name sources `*Test.cpp` and targets `<name>_test`.
- Tests are simple binaries; run via `bazelisk run //<path>:<target>`. If using frameworks (e.g., Abseil), follow existing library deps.

## Commit & PR Guidelines
- Commits: imperative and scoped (preferred Conventional Commits). Example: `feat(stitching): improve synchronization for dual-camera`.
- PRs: include purpose, configs/commands used to validate, relevant logs/output or screenshots, and linked issues. Note any platform constraints (Jetson vs x86).

## Security & Configuration Tips
- Do not commit new large binaries (e.g., TensorRT `*.engine`); store externally and reference paths in configs.
- Verify local paths in `WORKSPACE.bazel` before builds; mismatches cause include/link errors.

## Jetson Notes
- Environment: JetPack 6.x with DeepStream installed at `/opt/nvidia/deepstream/deepstream`.
- Flags: prefer `--config=jetson` (sets `aarch64` and Jetson-specific copts) or `--cpu=aarch64` explicitly.
- Memory: build specific targets when RAM is constrained, e.g., `bazelisk build --config=jetson //src/libs/scoreboard:scoreboard`.
- Debug: use `--config=gstdebug` for GStreamer-heavy debugging builds.

## Architecture Overview
- Apps: primary entrypoint is `pipeline-app` under `src/apps/pipeline-app`, which wires sources, inference, overlays, and sinks via GStreamer/DeepStream.
- Libraries: modular C++ in `src/libs/*`:
  - `camera/` (capture/control), `stitching/` (sync/correspondence), `draw_display/` (CUDA text/overlays), `scoreboard/` (rendering/logic), `gopro/` (BLE control), and `nvdsinfer_custom_impl_Yolo/` (YOLO parser/plugins).
- Stitching video discovery mirrors `hmlib/orientation.py`: supports GoPro and Insta360 chapter patterns, and camera-specific subdirectories named `cam1`, `cam2`, etc. under a game directory.
- CUDA: GPU kernels live next to libs (e.g., `*.cu`), built with Bazel `cuda_library` and linked into `cc_library` targets.
- External deps: resolved in `WORKSPACE.bazel` (DeepStream, GStreamer, OpenCV, Abseil, CUDA, jetson-utils, hm-cupano, magic_enum).
- Data flow (typical): sources → decode → inference/tracking → stitching/aggregation → overlays (draw_display/scoreboard) → sinks (encode/file/rtp).
- Configuration: runtime controlled by YAML in `configs/` and CLI `--options=key=value` flags.
