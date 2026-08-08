# In-Process UI and Runtime Control Design

## Status

Reviewed design; implementation started with the runtime type, GStreamer property-service foundation, and Qt control-surface
shell. The current Qt app uses a disconnected demo backend for testable UI wiring; the next implementation slice should
connect those actions to the real `PipelineController` backend.

UI visual target: [pipeline-ui-runtime-control-mockup.png](pipeline-ui-runtime-control-mockup.png). The editable source is
[pipeline-ui-runtime-control-mockup.html](pipeline-ui-runtime-control-mockup.html).

## Goals

- Add a desktop UI for the pipeline that runs the DeepStream/GStreamer pipeline in the same process.
- Embed the current `--show` view in the UI instead of launching a separate render window or child process.
- Allow runtime control of outputs: start/stop RTMP, RTSP server/client, WebRTC, file recording, and YouTube RTMP targets without stopping the pipeline.
- Expose camera/tracking/stitch/color controls similar to `../hm/hm-ui`, including runtime-adjustable values like stitch rotation.
- Keep arbitrary GObject/GStreamer plugin properties discoverable and editable without adding hand-written plumbing for every property.
- Preserve the current CLI as a supported headless entrypoint.
- Make the application packageable as Debian packages so an operator can install and run it with normal Debian tooling.
- Provide a Windows installer/launcher path that runs the UI-controlled Linux app through WSL for operators on Windows
  workstations.

## Non-Goals For The First Implementation

- Rewriting the DeepStream pipeline in another language.
- Replacing all YAML configuration with UI-managed state.
- Supporting arbitrary live graph edits from the UI as the primary operator workflow.
- Solving model/pretrained asset hosting in the UI package. The package should install code and static configs; large engines/assets remain declared and downloaded or provisioned separately.
- A native Windows port of the DeepStream/GStreamer pipeline. The Windows distribution path should use WSL and the same
  Linux packages/runtime.

## Existing Context

`pipeline-app` currently owns a C++ `GstPipeline` created in `create_pipeline()` and driven through `PipelineApplication`.
The installed CLI command should be renamed to `hstream-cli`, with `pipeline-app` kept only as a compatibility/developer
target while existing scripts migrate. It already has:

- A real in-process `GstPipeline` (`pipeline->pipeline = gst_pipeline_new("pipeline")`).
- Existing tee points that are useful but must be classified before runtime use:
  - `pipeline->tiler_tee` currently feeds the tiled display path and is not automatically the final preview image.
  - `pipeline->common_elements.tee` is after common analytics/message conversion.
  - A new explicit post-composition `display_tee` is likely needed for the final UI preview/output view.
- Runtime property changes in a few places, for example `show-source` on the tiler.
- A GTK prototype in `src/libs/common/ModPipeline.cpp` that introspects pipeline elements and edits writable GObject properties.
- A more complete Qt/GStreamer prototype in `src/apps/pstudio`, including property listing and property set logic.
- Generic property parsing/application in `src/apps/apps-common/gst_plugin_properties.{h,cpp}`.
- YAML `properties` and `private-properties` for several custom videoprep/playtracker/stitcher bins.

`../hm/hm-ui` is a Rust/egui sidecar. The important reusable idea is not the process model; it is the control metadata model:

- Runtime declares windows and controls dynamically.
- Controls have names, max values, current values, and defaults.
- Values can be applied live and saved back to config.
- Camera controls include braking, speed/accel limits, stitch rotation, and color controls for stitched/left/right paths.

For `hstream`, this same dynamic metadata should live in-process and be backed by a C++ controller API.

## Recommendation

Use a C++ Qt application host with a C++ pipeline controller library. The first UI target is Qt Widgets, because
`GstVideoOverlay` can use a stable `QWidget::winId()` and the repo already has Qt/GStreamer Widgets code in
`src/apps/pstudio`. The controller API must remain UI-toolkit independent so a Qt Quick/QML shell can replace or wrap the
Widgets UI later.

Qt/C++ is the lowest-risk UI choice for this requirement set because the hard integration points are
C++/GStreamer/DeepStream concepts:

- `GstElement*`, `GstPad*`, `GstBus`, and `GstVideoOverlay`.
- Native window handles for embedded video.
- GL/EGL/X11/Wayland lifetime and thread affinity.
- Dynamic tee branches with pad blocking and state synchronization.
- Debian packaging of native libraries, plugins, desktop files, and Qt runtime dependencies.

Rust + Slint remains a reasonable future UI option, but it would require a C++ FFI layer for the same pipeline control API. That boundary should not be introduced before the controller is stable.

## Proposed Module Layout

Add three layers:

1. `src/libs/pipeline_controller`
   - Owns `AppCtx`/`NvDsPipeline` lifecycle as a library.
   - Exposes typed operations for start/stop, preview attachment, dynamic outputs, camera controls, runtime property introspection, and status.
   - Contains no Qt dependencies.

2. `src/apps/pipeline-app`
   - Remains the CLI implementation, exposed as `hstream-cli`.
   - Becomes a thin wrapper around `pipeline_controller`.
   - Keeps existing flags and behavior.
   - Keeps CLI-only behavior such as terminal mode changes, stdin commands, signal handlers, progress UI, and standalone X11
     windows outside the reusable controller.

3. `src/apps/hstream-ui`
   - New Qt Widgets desktop app.
   - Depends on `pipeline_controller`.
   - Presents operator workflows: preview, sources, outputs, camera controls, logs, status, diagnostics.

This keeps the runtime control API reusable by future REST, headless service, or alternate UI frontends.

## Pipeline Controller API

The controller is the ownership boundary around the live pipeline.

```cpp
namespace hm::pipeline {

struct PipelineLaunchConfig {
  std::vector<std::string> config_files;
  std::string game_id;
  std::vector<std::string> input_uris;
  std::vector<std::string> enabled_sources;
  std::vector<std::string> enabled_sinks;
  std::vector<std::pair<std::string, std::string>> pipeline_options;
  bool one_pass_stitching = true;
};

struct RuntimeOutputSpec {
  std::string id;
  RuntimeOutputKind kind;
  int source_id = 0;
  std::string tee_point;
  std::string uri;
  std::string host;
  int port = 0;
  std::string mount_path;
  std::string output_file;
  int width = 0;
  int height = 0;
  int bitrate = 0;
  bool include_audio = true;
  bool uri_contains_secret = false;
};

class PipelineController {
 public:
  absl::Status configure(PipelineLaunchConfig config);
  absl::Status start();
  absl::Status stop();
  absl::Status pause();
  absl::Status resume();

  absl::Status attachPreviewWindow(uintptr_t native_window_id);
  absl::Status detachPreviewWindow();
  absl::Status setPreviewSource(int source_id);

  absl::Status addOutput(RuntimeOutputSpec spec);
  absl::Status removeOutput(std::string_view output_id);
  absl::Status setOutputEnabled(std::string_view output_id, bool enabled);
  std::vector<RuntimeOutputStatus> outputs() const;

  std::vector<RuntimeControlGroup> controls() const;
  absl::Status setControlValue(std::string_view control_id, RuntimeValue value);
  RuntimeValue getControlValue(std::string_view control_id) const;

  std::vector<GstElementInfo> elements() const;
  std::vector<GstPropertyInfo> properties(std::string_view element_path) const;
  absl::Status setProperty(std::string_view element_path, std::string_view property, std::string_view serialized_value);

  RuntimeStatus status() const;
};

} // namespace hm::pipeline
```

All methods that touch GStreamer should execute on the pipeline/main-loop context. The public API may be callable from Qt threads, but it should marshal work internally with `g_main_context_invoke()` or an equivalent command queue. The UI must not directly mutate `GstElement*`.

### Controller Extraction Boundary

`PipelineApplication` is not currently the controller. It mixes pipeline creation with CLI-only behavior such as singleton
process state, terminal mode, signal handling, blocking `g_main_loop_run()`, stdin runtime commands, and X11 event
threads. The first implementation must split these responsibilities:

- `PipelineRuntime`: creates/configures `AppCtx`, builds the `GstPipeline`, starts/stops state transitions, owns the GLib
  context, and exposes runtime handles.
- `PipelineController`: thread-safe facade over `PipelineRuntime`.
- `hstream-cli`: parses flags and wires terminal/progress/X11 behavior around `PipelineController`.
- `hstream-ui`: calls the same controller without inheriting CLI terminal or X11 behavior.

Existing functions such as `create_pipeline()` can be reused, but they must accept the controller's GLib context and avoid
installing watches/timeouts on the default context implicitly.

### GLib Context Ownership

The UI must not depend on accidental default-context integration between Qt and GLib. The controller owns a dedicated
`GMainContext` and one runtime thread:

1. Create `GMainContext` and `GMainLoop` in `PipelineRuntime`.
2. Push that context as thread-default while constructing the pipeline.
3. Attach bus watches, timers, RTSP servers, and command sources to this explicit context.
4. Marshal all public controller operations onto this context.
5. Emit status/control/output updates back to Qt through thread-safe callbacks or queued Qt signals.

Any existing `gst_bus_add_watch()`, `g_timeout_add()`, and `gst_rtsp_server_attach(..., NULL)` usage that is needed by
the reusable runtime must be converted to explicit-context equivalents. CLI-only helpers may continue using the default
context if they stay outside `PipelineRuntime`.

## Embedded Preview

The first preview implementation should use a dedicated render sink branch from a post-composition tee. Do not attach the
preview directly to `pipeline->tiler_tee`: in the current graph, that tee feeds the tiled display bin and may carry
pre-tiler batched buffers rather than the final `--show` image.

The preview source must be one of:

- Preferred: a new explicit `display_tee` inserted after tiler, OSD, and final video prep, immediately before configured
  display/encode sinks for the final view.
- Acceptable first slice: the existing sink-bin tee inside `instance_bins[0]` if inspection confirms it is downstream of
  tiler/OSD and carries the same buffers that the current render sink receives.

Dedicated preview branch:

```text
display_tee -> queue -> nvegltransform? -> nveglglessink
```

The Qt widget supplies a native window id. The controller sets that id on the video sink through `GstVideoOverlay`.

Design constraints:

- The preview branch is always controlled by `PipelineController`.
- The UI owns the Qt widget lifetime; the controller owns the GStreamer sink branch lifetime.
- If no UI window is attached, the branch should be disabled or use `fakesink`.
- The existing `--show` behavior should be implemented as the CLI equivalent of attaching a standalone `XWindow`.
- The controller installs a bus sync handler for `prepare-window-handle` messages and sets the handle before the sink
  reaches PLAYING.
- The UI reports widget resize events so the controller can call `gst_video_overlay_set_render_rectangle()` when needed.
- When the Qt widget is destroyed, the preview sink branch transitions to READY/NULL before the native window disappears.

Future work can add `appsink`/DMA-BUF/GL texture preview, but the first implementation should prefer `GstVideoOverlay` because it matches GStreamer/Qt practice and avoids copying frames.

## Runtime Output Graph

Dynamic outputs should be modeled as managed branches from stable tee points, not as ad hoc element editing.

Common branch shape:

```text
tee -> queue -> converter/scaler -> encoder -> mux/payloader -> sink
```

Output kinds:

- `RtmpPush`: `flvmux ! rtmpsink`, including YouTube when URI/key point to YouTube.
- `RtspServer`: local RTSP server mount backed by an RTP payloader branch.
- `RtspClient`: RTSP push/publish if supported by selected sink elements.
- `WebRtc`: existing WebRTC sink/signaling path.
- `FileRecord`: encoder/mux/filesink branch.
- `Preview`: UI render sink branch.

Output operator model:

- The UI shows an outputs table with `disabled`, `starting`, `live`, `reconnecting`, `draining`, `stopped`, and `error`
  states.
- Each output has a stable id, destination, branch kind, tee point, created time, bytes/frames if available, and last error.
- YouTube is a preset for `RtmpPush`; the stream key is treated as a secret and is never logged in full.
- RTSP server outputs have explicit host, port, mount path, and session status.
- File recordings have output path, duration, and finalization status.
- Branch failures do not stop the main pipeline unless the operator asks for fail-fast behavior.

Add/remove algorithm:

1. Validate output spec and check for id collision.
2. Build the branch in NULL state.
3. Add a queue at the branch head.
4. Request a tee src pad.
5. Install a blocking pad probe if modifying a running tee.
6. Add the branch to the pipeline bin.
7. Link tee pad to branch sink.
8. `gst_element_sync_state_with_parent()` on branch elements.
9. Remove pad block after branch reaches expected state.
10. Track tee pad, branch bin, and status by output id.

Runtime branches must not use helpers that discard the requested tee pad. The current startup helper pattern is fine for
static graph construction, but runtime output code must keep an owned/request-pad handle so it can block, unlink, and
release the pad during removal.

Remove algorithm:

1. Mark output as draining/stopping.
2. Block the tee src pad so no new buffers enter the branch.
3. Send EOS into the branch, not the whole pipeline, for sinks/muxers/encoders that need finalization.
4. Wait for branch-local EOS completion through a branch bus message, pad probe, or sink event observation.
5. On timeout, surface an error and allow force removal.
6. Set branch to NULL.
7. Unlink and release tee request pad.
8. Remove branch bin and side objects from the pipeline/context.
9. Emit status update.

The implementation should start with one video-only RTMP push branch and one preview branch, then add RTSP/WebRTC/file once the lifecycle helper is tested.

### RTSP Output Ownership

Current RTSP support is not a self-contained runtime output: it uses a pipeline branch to `udpsink` and separate global
`GstRTSPServer` state with fixed mounts. The runtime design needs a new per-output handle:

```cpp
struct RtspOutputHandle {
  std::string output_id;
  std::string mount_path;
  int port;
  GstElement* branch_bin;
  GstPad* tee_src_pad;
  GstRTSPServer* server;
  GstRTSPMediaFactory* factory;
  guint server_source_id;
};
```

Each RTSP output owns its server/factory/mount/session objects or references a shared server through explicit reference
counting. Removal detaches the mount/session and removes the GLib source from the controller's context. No runtime RTSP
code should use fixed global mount names such as `/ds-test`.

## Runtime Control Metadata

Use a typed in-process version of the `hm-ui` control schema.

```cpp
enum class RuntimeControlKind {
  Toggle,
  Integer,
  Float,
  Enum,
  Text,
};

enum class RuntimeControlApplyMode {
  Live,    // can be applied while PLAYING
  Paused,  // requires PAUSED
  Ready,   // requires READY/NULL
  Restart, // save-only until the next pipeline start
};

struct RuntimeControlDescriptor {
  std::string id;          // stable id, e.g. "playtracker.stop_direction_change_delay_frames"
  std::string group_id;    // "tracking", "stitching", "color.global", "source.cam1"
  std::string label;
  RuntimeControlKind kind;
  RuntimeValue min;
  RuntimeValue max;
  RuntimeValue step;
  RuntimeValue value;
  RuntimeValue default_value;
  std::vector<RuntimeChoiceValue> choices;
  std::string unit;
  double display_scale = 1.0; // raw value * display_scale is shown to the user
  int precision = 0;
  bool dirty = false;
  bool advanced = false;
  RuntimeControlApplyMode apply_mode = RuntimeControlApplyMode::Restart;
  bool live_writable = false;
  bool persisted = true;
  bool secret = false;
  bool unsafe = false;
  std::string source_id;
  std::string validation_error;
};
```

Control groups should be generated from:

- Explicit controller-defined runtime controls for high-value workflows:
  - Stop direction change delay.
  - Cancel stop on opposite direction.
  - Stop cancel hysteresis.
  - Stop delay cooldown.
  - Overshoot stop delay.
  - Post-nonstop stop delay.
  - Overshoot speed ratio.
  - Time-to-destination speed limit.
  - Apply to fast/follower box.
  - Max speed X/Y.
  - Max accel X/Y.
  - Stitch rotate degrees.
  - Global/left/right white balance, brightness, exposure, contrast, gamma.
- GObject property introspection for arbitrary element properties.
- Optional YAML metadata to group, label, scale, and persist selected properties.

The UI should render controls from descriptors, not hardcode every slider.

Apply modes:

- `live`: can be changed while the pipeline is PLAYING.
- `paused`: requires a PAUSED transition.
- `ready`: requires a READY/NULL transition or next run.
- `restart`: save-only until the next pipeline start.

The controller computes the apply mode from explicit control metadata and GStreamer property mutability flags, including
`GST_PARAM_MUTABLE_PLAYING`, `GST_PARAM_MUTABLE_PAUSED`, and `GST_PARAM_MUTABLE_READY`.

## Camera/Tracking Control Targets

The `../hm` camera UI applies values to Python `PlayTracker`, moving boxes, config dictionaries, and stitch/color processors. In `hstream`, equivalent live targets should be GStreamer plugin properties whenever possible.

Initial control-to-target matrix:

| Control area | Desired target | Current status | First implementation requirement |
| --- | --- | --- | --- |
| Stitch rotation | `hmstitcher`/`hmplaycropper` live GObject property | Mostly config/private-config driven | Promote to live property before advertising PLAYING-time adjustment |
| Play tracker braking/speed/accel | `vpplaytracker` live GObject properties | Many desired controls are not first-class properties | Add plugin properties and wire them to the live tracker state |
| Global color | final videoprep/playcropper color properties | Some values may exist only in private config | Promote high-value values to live properties; save private config only for defaults |
| Left/right stitch color | side-specific stitch/prep properties | Config/private-config driven | Promote to properties where the algorithm can update live |
| Source camera controls | source element or child property | Source-specific; CSI properties are set during source creation | Support one source type first, then extend through metadata |
| Arbitrary element property | any discovered GObject property | Writable-only helpers exist | Expose with mutability/apply-mode checks |

Values that remain serialized in `plugin-private-config` are not considered live controls. They may appear in the UI only
as save-only/restart-required settings. Runtime-adjustable controls must be real GObject properties or explicit
controller operations.

Source camera support should be phased:

1. Enumerate and display URI/file/RTSP sources as read-only runtime sources.
2. Add live controls for one camera source type used by production first, likely `nvarguscamerasrc` on Jetson or `v4l2src`
   on USB cameras.
3. Add a metadata file that whitelists source properties, display labels, ranges, mutability, and persistence behavior.
4. Extend to GoPro/Insta hardware controls only through their existing libraries or explicit source plugins, not by
   assuming file sources have live camera controls.

If an existing value only exists in serialized `plugin-private-config`, promote it to a real GObject property on the relevant plugin before exposing it as a live UI control.

## Arbitrary Property Editing

The repo already has two useful implementations:

- `hm::gst::set_plugin_property_from_string()` validates and sets typed GObject properties.
- `pstudio` lists and edits GObject properties.

The controller should consolidate this into a reusable property service:

- Enumerate element tree paths.
- List properties with type, min/max/default/current value, flags, and blurb.
- Filter out construct-only, read-only, and unsafe properties by default.
- Allow an "advanced" UI mode for arbitrary writable properties.
- Use `hm::gst::set_plugin_property_from_string()` for typed string application.
- Emit property-change status and errors.
- Mark properties as live/paused/ready/restart based on `GST_PARAM_MUTABLE_*` flags and current pipeline state. Plain
  `G_PARAM_WRITABLE` is not enough for a PLAYING-time UI.
- Block direct editing of properties known to destabilize DeepStream memory or GPU ownership unless explicitly whitelisted.

This gives the "add arbitrary new properties without modifying code everywhere" path:

1. Add a GObject property to a plugin.
2. Optionally add metadata to group/label/scale it.
3. The UI discovers it automatically.

## Persistence

Runtime changes need separate handling for live state and saved defaults.

- Live state: stored in controller memory and applied immediately to GStreamer elements.
- Session state: optionally saved under the game/output work dir for restore.
- Config persistence: writes a narrow override file, not the generated YAML wholesale.

Recommended path:

```text
$HM_GAME_DIR/<game_id>/hstream/runtime-overrides.yaml
```

This file is a UI/session override layer, not a replacement for existing generated/private configs. Load order:

1. Base YAML configs.
2. Existing generated/private config artifacts created by the configurator.
3. `runtime-overrides.yaml` for persisted UI changes.
4. CLI `--options` and explicit command-line flags.

Save semantics:

- The UI writes only controls marked `persisted=true`.
- If a value affects configurator invalidation, such as stitch rotation or canvas sizing, the Save path must call the same
  configurator invalidation/update code used by CLI-backed config changes.
- Reset restores runtime values to descriptor defaults; Save is a separate action.
- A save-only/restart-required value is written to `runtime-overrides.yaml` and surfaced as pending until restart.

## Debian Packaging

Add packaging as a first-class deliverable.

Suggested packages:

- `hmstream`
  - CLI binaries: `hstream-cli`, helper tools.
  - Runtime configs under `/usr/share/hstream/configs`.
  - Scripts that are safe as installed commands.
  - Depends on `hmstream-gst-plugins`, because the normal pipeline requires the custom GStreamer plugins.
- `hstream-ui`
  - Qt Widgets desktop app.
  - `.desktop` file and icon.
  - Depends on `hmstream`; it receives custom plugins transitively through `hmstream`.
- `hmstream-gst-plugins`
  - Custom GStreamer plugins under the appropriate multiarch GStreamer plugin directory.
  - Runs `gst-inspect-1.0` smoke check in CI/package validation where possible.
- `hmstream-dev` (optional)
  - Headers and C++ library artifacts for embedding/testing.

External dependencies:

- DeepStream, CUDA, TensorRT, NVIDIA driver, and Jetson-specific runtime packages should remain external dependencies or documented prerequisites. They are large, platform-specific, and usually installed from NVIDIA repositories.
- Debian package metadata should use `dpkg-shlibdeps`/`${shlibs:Depends}` for shared-library dependencies wherever
  possible, because Qt package names differ across Debian/Ubuntu releases. Hand-written dependencies should be limited to
  stable runtime packages and distro-specific packaging metadata.
- Package scripts should validate DeepStream presence and print actionable errors rather than silently failing.

Installed paths:

- `/usr/bin/hstream-cli`: CLI wrapper.
- `/usr/bin/hstream-ui`: desktop UI wrapper.
- `/usr/bin/hmstream-doctor`: diagnostics.
- `/usr/lib/hstream/`: private shared libraries and helper binaries that should not be on the public PATH.
- `/usr/lib/<triplet>/gstreamer-1.0/`: packaged hstream GStreamer plugins.
- `/usr/share/hstream/configs/`: installed runtime configs.
- `/usr/share/hstream/env/`: installed environment defaults and launch fragments.
- `/usr/share/applications/hstream-ui.desktop`: desktop entry.
- `/usr/share/icons/hicolor/.../apps/hstream.png`: icon.

Runtime discovery:

- Prefer RPATH/RUNPATH for hstream private libraries over broad `LD_LIBRARY_PATH`.
- Use the system GStreamer plugin directory for packaged plugins so `GST_PLUGIN_PATH` is normally unnecessary.
- The wrappers may prepend `GST_PLUGIN_PATH` only for nonstandard installs or developer overrides.
- DeepStream discovery order is: `DEEPSTREAM_ROOT`, `/opt/nvidia/deepstream/deepstream`, then package-specific config.
- Config discovery order is: explicit CLI/UI selection, `HSTREAM_CONFIG_ROOT`, `/usr/share/hstream/configs`, then repo-local
  paths only in developer builds.

Build approach:

- Keep Bazel as the source build.
- Add a packaging target/script that stages Bazel outputs into a Debian package root.
- Use `dpkg-deb`/`debhelper` for first packages; avoid inventing a custom installer.
- Include an installed environment wrapper that sets required DeepStream/GStreamer plugin paths consistently.
- Start with split packages: `hmstream`, `hstream-ui`, `hmstream-gst-plugins`, and optional `hmstream-dev`.

Installed commands:

- `hstream-cli` for the CLI.
- `hstream-ui` for the desktop UI.
- `hmstream-doctor` for dependency and plugin diagnostics.

Validation:

- `hmstream-doctor` checks NVIDIA driver, CUDA, DeepStream path, GStreamer plugin discovery, custom plugin discovery, config root, writable game root, and optional RTMP/RTSP/WebRTC dependencies.
- CI/package smoke tests should run `hmstream-doctor --no-gpu-required` and a short `FAKE` sink pipeline where dependencies are available.

## Windows Installer Via WSL

Add a Windows installer as a distribution wrapper around a validated Linux runtime, not as a separate Windows build of
the pipeline. NVIDIA's current DeepStream-on-WSL guidance is Docker-based and has documented display-sink caveats, so the
first Windows deliverable must be bounded to a tested WSL distro/container/display matrix before claiming embedded
preview support.

Recommended shape:

- Build a signed Windows installer with WiX Toolset or MSIX after the Debian packages exist.
- Install a small Windows launcher, Start Menu shortcut, icon, and optional PowerShell helper scripts.
- Detect WSL 2 and offer to install/enable it when the user has administrator rights.
- Install or select a supported Ubuntu/Debian WSL distro, then either run the validated DeepStream container path or
  install distro-specific `.deb` artifacts when that path is proven.
- Run `hmstream-doctor` inside WSL after installation and show actionable failures in the Windows installer UI/log.
- Launch the UI with `wsl.exe -d <distro> -- hstream-ui` only for validated WSLg/display-sink combinations.

Display and hardware assumptions:

- First target candidate is Windows 11 with WSLg, because Qt/X11/Wayland windows can appear on the Windows desktop
  without a separate X server.
- GPU acceleration depends on NVIDIA driver, WSL GPU support, CUDA for WSL, and DeepStream compatibility inside the
  selected distro. The installer should verify and report this; it should not silently fall back to an unsupported mode.
- If the selected DeepStream sink path cannot render under WSLg, installer validation should use file/FAKE sinks and mark
  live embedded preview unsupported for that matrix.
- Windows 10 can be documented as advanced/manual unless a supported X server path is explicitly validated.

Networking and device access:

- RTMP push to YouTube usually works from inside WSL without Windows port forwarding.
- RTSP/WebRTC servers need explicit bind/port behavior documented. The launcher should surface the WSL IP/port and, when
  feasible, configure Windows firewall rules for selected server ports.
- Local media paths should be selectable from Windows and translated to WSL paths (`C:\...` to `/mnt/c/...`) before
  invoking `hstream-ui`.
- Direct camera device access should remain a Linux/Jetson-first capability until each Windows/WSL camera path is
  validated. Network cameras and file inputs are the first Windows installer targets.

Installed Windows components:

- `HStream UI` Start Menu shortcut.
- `hmstream-launcher.exe` or PowerShell-backed launcher that invokes `wsl.exe`.
- `hmstream-wsl-doctor.ps1` for collecting WSL, GPU, package, and network diagnostics.
- Optional settings file under `%ProgramData%\HStream\launcher.json` for distro name, default game root, and exposed
  ports.

Validation:

- Fresh Windows 11 VM or workstation with WSLg.
- Installer provisions/selects distro, installs packages, and runs `hmstream-doctor`.
- `hstream-ui` opens from the Start Menu.
- FAKE/video-file pipeline starts inside WSL; embedded preview is required only for matrices with a validated display sink.
- RTMP push and RTSP server port behavior are tested from Windows-side clients.

## Implementation Phases

### Phase 1: Controller Foundation

- Extract a small `pipeline_controller` library around reusable `AppCtx`/`GstPipeline` startup.
- Keep CLI behavior unchanged.
- Add status snapshots and safe GStreamer main-context command dispatch.
- Add element/property introspection and typed property setting service.
- Tests: property introspection/set on simple GStreamer elements; CLI argument compatibility where feasible.

### Phase 2: Preview Host

- Add preview attachment API using native window id and `GstVideoOverlay`.
- Make CLI `--show` use the same preview sink path.
- Add minimal Qt app window that starts a configured pipeline and embeds preview.
- Tests: headless/unit for branch construction; manual DeepStream preview smoke.

### Phase 3: Dynamic Output Branches

- Add `RuntimeOutputManager` with tee branch lifecycle helpers.
- Implement RTMP push first, including YouTube target editing.
- Add remove/disable path with pad blocking and branch teardown.
- Extend to RTSP server, WebRTC, file recording.
- Tests: GStreamer-only tee branch add/remove with `videotestsrc`; integration smoke where DeepStream is available.

### Phase 4: Runtime Controls

- Add runtime control descriptors and Qt dynamic control rendering.
- Map camera/tracking/stitch/color controls to GObject properties.
- Promote any required plugin-private values to live GObject properties.
- Add Save/Reset using runtime override YAML.
- Tests: descriptor generation, value scaling, persistence round trip, plugin property application.

### Phase 5: Debian Packaging

- Add packaging staging script and Debian metadata.
- Produce split CLI/UI/plugin packages from the first packaging pass.
- Add `hmstream-doctor`.
- Package smoke validation.
- Keep `hstream-ui` disabled for Jetson builds until the Jetson sysroot/package build includes Qt6 development headers
  and the embedded preview path is validated there.

### Phase 6: Windows Installer Via WSL

- Add a Windows installer/launcher project after Debian packages and `hmstream-doctor` are available.
- Provision or target a supported WSL distro and install the same `.deb` packages there.
- Add Start Menu launcher, Windows-side diagnostics wrapper, path translation, and firewall/port guidance.
- Validate WSLg UI launch and file/RTMP/RTSP workflows on Windows 11.

## Risks And Mitigations

- Dynamic graph edits can deadlock or drop buffers if done directly from UI threads.
  - Mitigation: marshal all graph mutations through one GStreamer context and use pad blocking helpers.
- Some sink changes require encoder/muxer restart.
  - Mitigation: treat every runtime output as an independent branch; restart branches, not the main pipeline.
- Some plugin-private settings may not be live-safe.
  - Mitigation: expose only true GObject properties as live controls; convert high-value private settings into properties.
- Embedded EGL preview may differ across X11, Wayland, and Jetson.
  - Mitigation: start with X11/native window smoke, keep standalone `--show` fallback, document supported display backends.
- Debian users may not have NVIDIA/DeepStream prerequisites.
  - Mitigation: package `hmstream-doctor` and fail early with exact missing dependency messages.
- WSL users may have Windows, WSL, NVIDIA driver, CUDA, and DeepStream versions that do not line up.
  - Mitigation: make the Windows installer run the WSL-side doctor before claiming success, and keep Windows/WSL support
    bounded to validated OS/distro/driver combinations.

## Open Questions

- Which display backend is the first supported target for embedded preview: X11 only, or X11 plus Wayland?
- Which GStreamer tee point should each output use by default: final tiled/stitch view, per-source demux output, or both?
- Which live controls are already real GObject properties in `vpplaytracker`, `hmstitcher`, and `hmplaycropper`, and which must be promoted?
- Should Debian packaging produce separate x86_64 and Jetson packages from the same metadata, or separate package names?
- Which Windows 11, WSL distro, NVIDIA driver, CUDA for WSL, and DeepStream version matrix should be the first supported
  installer target?
