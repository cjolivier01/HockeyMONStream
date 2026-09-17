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
- ARM64/SBSA build (native non-Jetson, e.g. GB300): `make arm64` or `bazelisk build --config=opt --config=arm64 //...`.
- Jetson (aarch64) build: `bazelisk build --config=jetson //...`.
- Before opening or updating PRs, validate changes on this machine with the x86_64 build and also validate ARM64/Jetson builds. A Jetson host is available at `stubby` for Jetson-specific build checks.
- Canonical runs:
  - End-to-end wrapper (recommended): `./run.sh --game-id=<game_id> -t=5`
    - Runs stage `0` (main pipeline) by default; if stitching artifacts are missing, one-pass stitching configures them in-process before allocating the `hmstitcher` output pool.
    - To run the older two-stage flow, pass `--two-stage` (stage `-1` stitching/rink-mask configuration with `FAKE`, then stage `0` main pipeline).
    - Supports `-t N`, `-t=N`, or `--time-limit=N`.
    - If configured pretrained assets are missing, it will download the assets declared in YAML `pretrained-assets`.
  - Direct `hstream-cli` invocation (useful for debugging configs):
    - Display only: `bazel-bin/src/apps/hstream-cli/hstream-cli -g <game_id> --enable-sources=URI-MULTIPLE --enable-sinks=RENDER --options=pipeline.hmaudio.enable=1`
    - Encode to file: `bazel-bin/src/apps/hstream-cli/hstream-cli -g <game_id> --enable-sources=URI-MULTIPLE --enable-sinks=ENCODE_FILE --options=pipeline.hmaudio.enable=1`
    - Fake sink (no UI): `bazel-bin/src/apps/hstream-cli/hstream-cli -g <game_id> --enable-sources=URI-MULTIPLE --enable-sinks=FAKE --options=pipeline.hmaudio.enable=1`
    - Explicit two-stage debugging: add `-c configs/ds_hockey_configure_stitching.yaml -c configs/ds_hockey_app_config.yaml`.
    - Multi-sink (comma-separated): `... --enable-sinks=RENDER,ENCODE_FILE`
    - All commands support an optional time limit: append `-t N` (or `--time-limit=N`) to stop after processing `N` seconds of video.
- Run a test binary (pattern):
  - `bazelisk run //src/libs/scoreboard:scoreboard_test`

Notes:
- This repo assumes CUDA/DeepStream/OpenCV and other system headers are installed; see `WORKSPACE.bazel` for local_repository paths (e.g., DeepStream under `/opt/nvidia/deepstream/deepstream`).
- Some DeepStream components have extra runtime shared-lib deps (e.g. tracker needs `libmosquitto1`).
- Game directories default to `$HOME/Videos/<game_id>`. Override with `HM_GAME_DIR=/path/to/games_root` (game dir becomes `${HM_GAME_DIR}/<game_id>`).
- The canonical default config is the bundled `configs/baseline.yaml`; keep it byte-for-byte synchronized with HockeyMOM's `hmlib/config/baseline.yaml`. Override explicitly via `HM_CONFIG_ROOT=/path/to/config` for development; invalid overrides fail startup rather than falling back.
- Install Bazelisk once via `scripts/install_bazelisk.sh` (the `bld` script will prompt if missing).
- One-pass stitching delays `hmstitcher` output caps and buffer-pool allocation until the first input batch generates/loads control masks and reveals the actual stitched canvas size. If CUDA OOM appears in one-pass runs, inspect the runtime canvas dimensions and downstream caps/pool reallocation path rather than tuning guessed width/height padding.

## Coding Style & Naming
- Language: C++17 (see `.bazelrc`); format with `.clang-format`.
- Indentation: 2 spaces, no tabs; line length ≈ 120; left-aligned pointers; sorted includes.
- Bazel: use `cc_library` for reusable code and `cc_binary` for tools/tests. Prefer `INCLUDE_PREFIX` so includes look like `#include "hstream/src/libs/<mod>/Header.h"`.
- File names: `PascalCase` for classes/headers, `snake_case` for Bazel targets when consistent with neighbors.

## GPU Data-Path Guidelines
- Keep video frames GPU-resident whenever possible. Avoid device-to-host (D2H) copies, CPU mapping/readback, and GPU-to-CPU-to-GPU round trips unless they are functionally required.
- Preview and display paths should prefer zero-copy GPU-native interop (for example NVMM/EGL/OpenGL/CUDA-compatible rendering) over conversion to system-memory images.
- When a D2H transfer is unavoidable, isolate it from the main pipeline, bound its frequency and resolution, and document why the transfer is necessary and what prevents a GPU-native path.
- Treat new system-memory caps, `gst_buffer_map`/`gst_video_frame_map` calls on video frames, CPU-side image conversion, and snapshot encoding in a steady-state video path as performance-sensitive changes that require explicit justification and measurement.

## Testing Guidelines
- Keep tests small and colocated. Name sources `*Test.cpp` and targets `<name>_test`.
- Tests are simple binaries; run via `bazelisk run //<path>:<target>`. If using frameworks (e.g., Abseil), follow existing library deps.

## Commit & PR Guidelines
- Commits: imperative and scoped (preferred Conventional Commits). Example: `feat(stitching): improve synchronization for dual-camera`.
- PRs: include purpose, configs/commands used to validate, relevant logs/output or screenshots, and linked issues. Note any platform constraints (Jetson vs x86).
- PR default: open normal (ready-for-review) PRs. Only open draft PRs when explicitly requested.
- Review cycle: after opening a PR, ask two high-reasoning Codex subagents to review the current PR in parallel when the subagent tool is available. Address actionable findings, then ask another two high-reasoning subagents to review the updated PR. Repeat this review/fix cycle until reviewers report no necessary fixes or only non-blocking suggestions remain; if reviewers or tools are unavailable or repeatedly stall, document that limitation and the completed validation instead of blocking indefinitely. Summarize each round's findings, fixes, and remaining risks in the PR or handoff.
- Related repositories: changes needed in HockeyMON, hm-cupano, and jetson-utils may be made in their respective repositories and submitted as PRs. If a repository is a fork, target `master` in the user's fork; otherwise, target `master` in that repository. Apply the same review/fix iteration requirements above to each PR. These review cycles may run in parallel with continued work in this hstream repository.

## Security & Configuration Tips
- Do not commit new large binaries (e.g., TensorRT `*.engine`); store externally and reference paths in configs.
- Verify local paths in `WORKSPACE.bazel` before builds; mismatches cause include/link errors.

## Jetson Notes
- Environment: JetPack 6.x with DeepStream installed at `/opt/nvidia/deepstream/deepstream`.
- Target split:
  - `--config=jetson`: Jetson-only build/cross-build path (Jetson sysroot/toolchain assumptions).
  - `--config=arm64`: non-Jetson ARM64/SBSA path (for example GB300), defines `AARCH64_IS_SBSA`.
- Cross-compiling:
  - Use `make jetson` for x86_64 -> Jetson cross-builds after syncing a Jetson sysroot (see `docs/jetson-cross-build.md`).
  - `make arm64` is for native non-Jetson ARM64/SBSA hosts, not x86_64 cross-compiles.
- Memory: build specific targets when RAM is constrained, e.g., `bazelisk build --config=jetson //src/libs/scoreboard:scoreboard`.
- Debug: use `--config=gstdebug` for GStreamer-heavy debugging builds.

## Architecture Overview

### Entry points and process ownership
- The main user-facing entry points are `hstream-ui` (desktop UI) and `hstream-cli` (CLI). HockeyMONStream uses C++17/GStreamer/DeepStream; runtime names remain `hstream-*`. The CLI source and single executable target live in `src/apps/hstream-cli/` (`//src/apps/hstream-cli:hstream-cli`).
- `src/apps/hstream-cli/PipelineApp.cpp` owns CLI startup, stage execution, the GLib main loop, runtime commands, seeking, and shutdown. `configurator.cpp` resolves configuration and prepares source, calibration, model, and output settings. `deepstream_app.cpp` assembles and tears down the graph using `AppCtx`/`NvDsPipeline` from `deepstream_app.h` and the bin builders in `src/apps/apps-common/`.
- The Qt desktop app is `src/apps/hstream-ui/`. Its `HStreamWindow.cpp` currently launches `hstream-cli` with `QProcess`, sends runtime commands through stdin, and consumes process output for status and acknowledgments. Preview windows are embedded using native window IDs. Pipeline mutations belong in the runner's runtime handlers; UI controls must honor their acknowledgments and stage/run generations.
- `src/libs/pipeline_controller/` currently provides runtime types and GStreamer property/graph inspection (`GstPropertyService`), not an in-process pipeline owner. `docs/in-process-ui-runtime-control-design.md` describes a proposed architecture; verify implemented behavior in the source before treating its phases as complete.
- `src/apps/hstream-job/` exports saved jobs as Bash scripts that invoke `hstream-cli`; `src/apps/hstream-assets/` and `src/libs/assets/` manage declared pretrained assets. `src/apps/dual-record/` is a separate camera-recording application.

### Video graph and implementation map
The normal enabled Program path is below; calibration-only stages and optional elements change the graph. `create_common_elements()` in `deepstream_app.cpp` builds links from downstream to upstream, so its source order is the reverse of video flow.

```text
camera files / live sources -> decode and batch -> hmstitcher
  -> primary inference -> field mask -> object tracker -> vpplaytracker
  -> playcropper (Program view/overlays) -> display / archive / network sinks
```

| Responsibility | Start reading here |
| --- | --- |
| Source discovery, chapter transitions, audio alignment, and decoding | `src/apps/apps-common/deepstream_source_bin.cpp`, `UriPlaylistDiscovery.h`; `src/libs/stitching/Orientation.cpp`, `Synchronization.cpp` |
| Lossless URI batching and demux | `src/apps/apps-common/HStreamLosslessMux.cpp`, `HStreamBatchDemux.cpp`, and `nvstreammux_lossless*.patch` |
| GPU transform framework and plugin registration | `src/gst-plugins/gst-videoprep/gstvideoprep.cpp`, `videoprep_plugins.cpp`, `algorithm-base/` |
| Panorama, camera policy, and Program crop | `src/gst-plugins/gst-videoprep/stitcher/`, `playtracker/`, `playcropper/` respectively |
| Field masking, native tracker adapter, detection parser | `src/gst-plugins/gst-fieldmask/`, `src/gst-plugins/gst-playtracker/PlayTrackerCtx.cpp`, `src/libs/nvdsinfer_custom_impl_Yolo/` |
| Rendering, scoreboard, GPU preview, and encoding | `src/libs/draw_display/`, `src/libs/scoreboard/`, `src/apps/apps-common/HmGpuPreview.cpp`, `deepstream_sink_bin.cpp`, `EncoderDimensions.cpp` |
| Shared frame metadata and configuration helpers | `src/libs/common/`; in particular `DecodedFrameSequenceMeta`, `DetectionSnapshotMeta`, `PreviewOverlayMeta`, `BaselineConfig`, and `UserConfig` |

- `hmstitcher`, `vpplaytracker`, and `playcropper` are variants registered by the shared `videoprep` plugin and selected by `plugin-type`. The Program path's `vpplaytracker` uses `PlayTrackerCtx`; the separate `playtracker` element is registered in `gst-playtracker/`. CUDA kernels live alongside their owning libraries/plugins and use Bazel `cuda_library` targets.
- URI-MULTIPLE discovery supports GoPro/Insta360 chapters and `cam1`, `cam2`, etc. directories, mirroring HockeyMON's `hmlib/orientation.py`. The playlist frame barrier and `hstreamlosslessmux` preserve synchronized decoded-frame sequences across chapter changes. Preserve sequence metadata, cancellation, and EOS behavior when changing batching or seeking; timeout-based partial batches or leaky queues can break recording completeness.
- The stitched archive has a separate route from the Program crop; GPU previews also have their own routing/overlay controls. Trace the actual tee and surface format before changing a sink. See `docs/stitched-archive-resolution.md` for native canvas versus encoder limits and the GPU Data-Path Guidelines above for memory-transfer constraints.

### Configuration and calibration state
- Canonical values resolve from bundled `configs/baseline.yaml` → `~/.hstream/hstream.yaml` → the game's `config.yaml` → CLI overrides. Structural app YAML (`configs/ds_hockey_app_config.yaml` and the optional configuration-stage YAML) supplies topology and native properties. `Configurator` tracks explicit layer precedence when translating canonical keys to native plugin properties; a direct native property wins over its canonical mapping at the same explicit layer. Extend the existing mapping instead of adding an independent set of defaults in a plugin or UI.
- `src/libs/common/BaselineConfig.cpp` locates the baseline for source/runfiles/package execution and validates `HM_CONFIG_ROOT`. `UserConfig.cpp` owns user configuration and path roots: `paths.game-root`/`HM_GAME_DIR` and `paths.output-root`/`HM_OUTPUT_WORK_DIR`; working output defaults to `~/hstream_output/<game-id>`. Game input/configuration, working output, and model caches have different lifetimes; resolve them through the existing helpers.
- `src/libs/stitching/ConfigureStitching.cpp` coordinates native calibration and artifact loading. `FeatureMatcher`, `HuginProject`, `HomographyMaps`, `RinkSegmentation`, `RinkLeveling`, and `CalibrationModels` implement matching, projection/mapping, rink analysis, and model use; `src/libs/onnx/` supplies ONNX runtime support. Production calibration runs without Python, though offline conversion and reference/parity tests may use it.
- Calibration publishes related artifacts in the game directory, including Hugin `.pto` projects, mapping TIFFs, `panorama.tif`, and `rink_mask_0.png`, with generation state in `config.yaml`. Use the locking/publication helpers in `GameConfig`, `TransactionState`, and `ConfigureStitching` for updates and invalidation; readers must not combine artifacts from different generations. `LiveStitchingGeneration`, `LiveOutputEpoch`, and `StitchedOutputGenerationPayload` coordinate authorized live geometry changes and frame metadata. Scoreboard and rink geometry must remain tied to the matching stitched output.
- One-pass startup derives the real canvas from calibration/artifacts before allocating output buffers. Caps negotiation and buffer-pool ownership cross `stitcher/`, `gstvideoprep.cpp`, and `algorithm-base/RuntimeOutputCaps`; keep these coordinated when changing geometry. Details: `docs/dynamic-one-pass-stitching.md`, `docs/native-feature-matchers.md`, `docs/projection-crop-selection.md`, and `docs/rink-leveling-selection.md`.

### Recording, replay, and publication
- DriveGPT recording uses a SQLite database, implemented by `src/libs/recording/` and `src/gst-plugins/gst-videoprep/playtracker/PlayTrackerTelemetryDb.*`. `telemetry-csv-dir` is a compatibility alias for database recording; legacy CSV readers/export code still exist. `src/libs/recording/schema.sql` is shared with HockeyMON's `hmlib/telemetry/schema.sql` and must remain compatible across producers/readers.
- Telemetry captures CPU detection/track metadata, native replay inputs/checkpoints, configuration, and geometry identity. Its bounded writer queue applies backpressure instead of dropping samples; errors fail recording. A database becomes complete only after successful pipeline shutdown/finalization. Preserve run UUIDs, sample/seek/reset boundaries, and geometry revisions; this path must not add video-surface readback. See `docs/telemetry-database.md`.
- `src/libs/playtracker_replay/` restores recorded native camera state for reproducible trials; `src/apps/hstream-ui/CameraExperiment*` provides experiment controls and GPU video preview. See `docs/camera-experiments.md` and `src/libs/playtracker_replay/README.md` for replay/provenance requirements.
- The CLI leaves archive MKVs and databases in working storage. On successful UI runs, `HStreamWindow.cpp` finalizes video through an ffmpeg remux and `TelemetryDbPublisher` publishes the completed database to the game directory using matching output suffixes. Exported `hstream-job` scripts run the CLI directly and do not perform that UI publication step. Do not equate pipeline EOS with completion of final-file publication.

### Dependency and distribution boundaries
- `WORKSPACE.bazel` pins `@hm` (HockeyMON's native `hockeymon/csrc/play_tracker`), `@hm-cupano` (panorama/CUDA mapping and blending), and `@jetson-utils` (GPU/video utilities). Changes in sibling checkouts do not affect these pins unless an explicit repository override is used. Inspect both the local adapter and the pinned external implementation for changes crossing these boundaries.
- External SDKs and libraries (DeepStream, CUDA/TensorRT, GStreamer, OpenCV, ONNX Runtime, Qt) are resolved through `WORKSPACE.bazel`, `bazel/`, and `buildfiles/third_party/`, using system installations or fetched dependencies as declared there. `//src/gst-plugins:hstream_runtime_plugins` supplies runtime plugin data; CLI/UI startup also configures plugin discovery for Bazel and installed runs. A linked application alone does not prove that the intended plugin `.so` is loaded.
- `scripts/make_deb*.sh` and `env/debian-package/` assemble `/opt/hstream` installations. Windows packaging in `packaging/windows/` runs the Linux stack in WSL; see `docs/windows-wsl-installer.md`. Keep the x86, Jetson, and native ARM64/SBSA split described above when changing build or runtime paths.

## Keeping Architectural Context Current
- Update this architecture overview in the same change that alters entry points, process/thread ownership, pipeline order, module responsibilities, configuration precedence, persistent artifacts/schema, cross-repository interfaces, or platform/package boundaries.
- Before editing, verify the affected description against implementation and `BUILD.bazel`/`WORKSPACE.bazel`; distinguish current behavior from proposed designs. If a design document is ahead of implementation, label that explicitly and link the current owner.
- Keep this file a concise navigation map with stable paths, ownership, and invariants. Put detailed algorithms, schemas, experiments, and troubleshooting in focused `docs/` files and link them here. Replace stale statements and links when code moves or behavior changes; avoid accumulating parallel descriptions, transient commit hashes, or session logs.
- When configuration or persistence changes span HockeyMON, hm-cupano, or jetson-utils, check the corresponding shared defaults/schema and dependency pins, then update the relevant documentation in each affected repository. Preserve the baseline synchronization requirement above.
- During validation/review, check that every new architectural path exists and that the documented flow, configuration precedence, and completion semantics still match the code. Include the documentation update in the PR description, or explain why an architecture-affecting change leaves this map accurate. Keep the existing Commit & PR Guidelines review cycle unchanged unless explicitly asked to revise it.
