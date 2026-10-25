# Shared desktop backends

`hstream-ui` is the operator desktop for both HockeyMONStream and HockeyMON.
Its game browser, calibration and rink editors, experiment dialogs, highlights,
camera controls and pipeline inspector have one implementation. The default
Program runner remains `hstream-cli`. HockeyMON selects its Python pipeline with:

```sh
hstream-ui --backend=hockeymon --backend-runner=/absolute/path/to/hm-cli \
  --game-id=example --play
```

`hm-ui` in HockeyMON supplies that selection. `--backend-launch` accepts a private
initial-launch document so `hmtrack --camera-ui=1` can preserve explicit Python
arguments. It applies once, to the game selected at startup, and is not copied
into exported jobs. A later Play uses the current saved and unsaved UI choices.
Initial Program envelopes include source ownership resolved by the Python
adapter before Qt opens, bound to the same game, arguments and working directory.
Qt checks it before calibration cleanup or crop admission. A single-source CLI
override therefore preserves the saved Left/Right calibration even when the
selected game's normal inputs are a camera pair. Unresolved initial envelopes
are rejected before changing calibration; recreate them through the matching
`hmtrack --camera-ui=1` launcher.
`--stitching-calibration-only` selects the native calibration workflow. When an
initial `hmstitch` envelope is present, `hm-cli` first translates those original
arguments and then executes the matching native runner, retaining native preview
ownership. Later calibration runs use the native runner directly.
`--help` does not require a display server.

The shared launcher uses `~/.config/hstream-ui/defaults.yaml` when present.
Only storage paths survive from the legacy user file; an empty defaults document
resets tuning to the baseline. Game settings override this user layer. Python
Players drawing changes apply on the next run; native Program drawing can also
change during playback. HockeyMON offers the supported standard analytics and
custom contracts; hockey pose/action models remain native-only. Its appearance
menu retains both DeepStream and NVIDIA ReIdentificationNet selections. Encoded
rink mini-maps currently require native Program processing; Python launch rejects
`output.rink-map.enable` instead of silently omitting the requested overlay.

## Ownership

The backend selection changes Program processing. Calibration, leveling, rink
mapping, stitching experiments, camera experiments and highlights continue to
use the shared native workflow libraries and matching native tools. There is no
second set of Rust widgets or Python copies of the desktop editors. The Python
adapter receives `HSTREAM_NATIVE_RUNNER` and `HM_HSTREAM_CALIBRATION_EXPORT` from
the matching installation or Bazel build. Native calibration artifacts are
validated and copied by `hstream-calibration-export` before Python consumes them.
The exporter holds HStream's calibration/configuration locks, checks the completed
owner and native provenance, preserves fractional synchronization offsets, and
copies the maps and validated rink mask into a private per-run directory. Its
versioned manifest binds source recordings and checksums each copied artifact.
HockeyMON verifies that manifest before loading maps and keeps renderer caches
and replacement masks out of the native game's calibration files.

`hstream-cli --settings-overlay FILE` applies a root YAML mapping for one run
after the saved configuration and structural application file. It preserves
ordered chapters and array-valued settings, including paths containing commas
or equals signs. Explicit `--options` still win. The overlay must be a regular
file of at most 16 MiB and does not save ordinary preferences to the game.
`-c` continues to mean an application pipeline, not a settings overlay.

The same helper exposes bounded scoreboard snapshot/save operations and live
rotation authorization validation. HockeyMON single-source runs keep their reference
canvas in the private backend session. The Qt selector uses the session's pinned
PNG and corners, never a saved pair's `s.png`. Native save verifies the source
playlist under the game config lock before persisting to a saved single-source
game. Temporary source overrides retain their selection only for the current run,
including seeks. Save requests may carry an absolute `CLOCK_MONOTONIC` deadline;
native canvas validation and transaction lock acquisition poll that deadline,
then check it under ownership immediately before publication. Expiry before
mutation returns an explicit `unchanged` result. Once publication starts, only
its result determines whether the selection changed. Callers must retain timed-out
requests and prevent Cancel from claiming no change while the outcome is pending,
unknown or already committed.
Once Python's frame-owner mailbox is ready, its pause receipt
includes `mode=cooperative` only after the frame owner stops advancement. Qt keeps
the supervisor alive to serve selection and other runtime requests; resume also
waits for the frame-owner receipt. Startup and native pauses retain SIGSTOP and
require resume before selection. An accepted rotation is applied with its
scoreboard epoch; each produced frame carries its exact native output generation
for telemetry and replay. Native rink maps are loaded and fitted through
`LoadPlaybackRinkMap`; Python receives the fitted plane and bounded viewing-ray
grid, without refitting the operator's evidence. Missing, stale or partial maps
fail when rink-aware framing or the mini-map requests them.


HockeyMON detector choices and prepared ONNX assets live under
`hstream_ui.hockeymon.detector` (`model`, `precision`, `engine`, `manifest`). They
do not replace native TensorRT paths in `pipeline.primary-gie` or
`hstream_ui.detector_int8`. The shared INT8 preparation dialog calls
`hm-cli --prepare-detector` for HockeyMON and retains the native preparer for
HockeyMONStream. Model preparation succeeds only after the returned model and
manifest exist; cancellation leaves the previous detector selected.

## Process protocol, version 1

Before opening the HockeyMON desktop, the UI invokes
`hm-cli --ui-backend-info`. It requires one JSON object:

```json
{"protocol":"hstream-ui","version":1,"backend":"hockeymon","preview":"jpeg-files"}
```

The CLI adapter accepts the same launch arguments generated by the desktop and
translates the existing stdin commands and output records. In particular,
runtime property batches, process pause/resume and seeks retain their existing
acknowledgments and generation checks. A successful write to stdin does not
mean the operation was applied. Pipeline inspection uses the shared inspector
protocol described in [pipeline-inspector.md](pipeline-inspector.md).

`src/libs/program/ToneControls.*` is the shared, Qt-free specification and
property builder for shadow lift, black-point lift and exposure. Qt reads its
labels/ranges and uses its validated presentation-unit conversion when building
the existing property batch. The active run's resolved precision chooses the
stitcher or Program cropper; changing next-run precision does not retarget a
running batch. The shared specification carries no default values and leaves
Qt's initialization and saved-setting resolution unchanged.
Eligibility, debounce, seek blocking and acknowledgments stay with the runtime
owner. The builder neither applies changes nor saves presets.

This extraction adds no browser endpoint and no AMD runtime command. Web form
settings remain admission-time inputs. A future live endpoint must validate
backend capability and correlate the run, stage, topology generation and request
before applying a typed control; browsers must not supply raw property commands
or native filesystem paths. Legacy property acknowledgments alone do not supply
that identity. `ProgramRuntimeControl` continues to own AMD command dispatch;
the shared builder does not make unsupported commands available there.

Each Program launch receives a fresh private directory in
`HSTREAM_UI_BACKEND_SESSION`. `HM_UI_INITIAL_LAUNCH`, when present, names the
one-use launch document. The selected runner owns its Python workers within the
UI's existing process group; stopping or closing the desktop also stops those
workers. Native workflow runners retain the existing runtime environment and
GPU preview path.

Before processing, `HM_BACKEND_LAUNCH` reports the resolved sinks and telemetry
request. For the original Python CLI launch, these replace the UI's initial
route choices and its frozen publication requirements; ordinary launches must
match the desktop request. The desktop uses the backend's cooperative `q` Stop
command for Python Program, with process-group termination reserved for timeout.
Cancellation during native preparation carries `status: cancelled` and
`stage: preparing`; it is accepted only after operator Stop and reports no files.

## Python preview transport

Python publishes atomic JPEG replacements in the private session:

| Channel | File |
| --- | --- |
| Program | `preview-final.jpg` |
| Stitched | `preview-stitched.jpg` |
| Camera 1 (single source or left camera) | `preview-camera-1.jpg` |
| Camera 2 (right camera) | `preview-camera-2.jpg` |

The UI polls only the selected channel, at most ten times per second, and does
not decode unchanged files. Python publishes only that selected channel, at
most fifteen times per second. Camera tabs retain a bounded copy of decoded
pixels before grading or overlays; single-source games publish Camera 1.
Render-off and the Pipeline inspector select no
image channel. Files must be regular JPEGs, not symlinks, of at most 8 MiB, no
dimension over 4096 pixels and at most 4,194,304 pixels. These are protocol
allocation bounds; the producer normally sends smaller previews. Decode failures
appear in the runtime log and preview status. The last frame remains available
after Stop, while the ordinary window layout is restored. The next run discards
the old session, and closing the desktop removes it.

This compatibility transport retains HockeyMON's existing bounded preview
readback. Python resizes GPU tensors before the host transfer and publishes
outside its processing worker. There is no shared CUDA/EGL allocation contract
between the Python pipeline and the Qt process. HStream's native path continues
to render GPU frames directly into X11 windows; it does not use this transport.
`NativePreviewSurface` explicitly switches pixel ownership when entering and
leaving Python image mode, including across calibration/Program runs.

## Publication and job export

HockeyMON owns publication of its recordings and telemetry. The UI does not
create a native publication manifest or run the native finalizer for Python
Program outputs. After publication the adapter emits:

```text
HM_BACKEND_PUBLICATION {"version":1,"status":"complete","game_id":"example","artifacts":[{"kind":"program","path":"/games/example/example_program-1.mp4"}]}
```

Artifact kinds are `program`, `stitched`, `program-4k` and `telemetry`. Paths must
name existing, nonempty regular files using absolute paths. Explicit output or
deployment destinations may be outside the game directory; the desktop only
displays these receipts and never performs native cleanup on their paths.
Requested recordings and telemetry must be present. A display-only run with
telemetry disabled may return an empty list. A failure receipt uses
`"status":"failed"` and an `error` message. A normal process exit without a
valid completed receipt is an error, not a successful recording.

For HockeyMON Program exports the existing Save job script flow calls
`hm-cli --export-job --game-dir DIR --output PATH --force`, with each SBATCH
directive passed as a separate `--sbatch` argument. The current choices are saved
before export. The generated job resolves `hm-cli` on the execution host and
does not retain a private UI session. Both exporters request one node, one task
and one GPU before operator SBATCH directives, and preserve the selected working
output root. Stitched-only exports and native Program
exports continue through `hstream-job`.

Focused validation targets are `//src/apps/hstream-ui:backend_options_test` and
`//src/apps/hstream-ui:shared_backend_window_test`; the existing native desktop
and X11 interaction suites remain regression coverage for the default backend.

## Shared model preparation

`hstream-model-prepare` exposes the same `PlayerModelCache` used by native
playback. The HockeyMON supervisor merges structural nodes from the native
application configuration into its resolved `pipeline` and launches this helper
under its process guard before starting Python inference:

```sh
hstream-model-prepare --config /private/session/model-input.yaml \
  --config-directory /opt/hstream/configs \
  --output /private/session/model-resolved.yaml \
  --receipt /private/session/model-receipt.json
```

The input may be a full configuration containing `pipeline`, or that pipeline
mapping alone. Output always contains `pipeline`; unrelated full-config fields
are preserved. Existing output and receipt paths are refused. A version-1 JSON
receipt has `status: complete`, the absolute `output` path and a `prepared` array.
Each entry identifies `feature`, `model`, `bundle`, `manifest`, `engine`, and the
available `onnx` or `etlt` source plus `tracker_config` for ReID. The immutable
bundle manifest remains authoritative for preprocessing, source identity and
engine bindings. Disabled and custom selections retain their configured paths
and produce no prepared entry.

Enabled analytics and ReID materialize `pipeline.player-analytics.gpu-id` and
`pipeline.tracker.gpu-id`, respectively, including custom selections. Resolution
uses the cache's existing per-group/application inheritance. Python retains
those accepted devices with the assets across seek rebuilds, carrying them to
pose, jersey, action, team and ReID ONNX CUDA providers or native inference.
Pose binds both CUDA inputs and outputs to the accepted device; only bounded
crops move if preprocessing uses a different GPU. Existing explicit CPU
execution and diagnosed automatic CPU fallback remain available.

HockeyMON's supervisor freezes the accepted bindings in a private, versioned
`accepted-models.json` tied to its session and game. Its worker reapplies those
bindings after original YAML/CLI overrides, checking model/custom-path identity
and retaining captured relative custom-path spellings. Other Python model and
control choices remain outside that snapshot; seeks reuse it without persisting
it into user configuration.

This prepares the selected model exactly. In particular, native default ReID is
DeepStream's pinned `resnet50_market1501.etlt` model and its SDK preprocessing
profile; the separately selectable ONNX ReIdentificationNet is not a substitute.
The existing native cache owns SHA verification, GPU/runtime identity, locking,
engine generation and cancellation. Both files publish only after preparation
succeeds. SIGINT, SIGTERM, stdin `q`/`quit`, or a bound direct parent's death
cancel preparation; stdin EOF allows batch jobs to continue. The optional
`--parent-pid` or `HSTREAM_UI_PARENT_PID` must identify the helper's direct parent.
The enclosing process guard still owns abrupt-exit descendant cleanup.

Bazel CLI/UI runtime data and desktop, Jetson and native ARM64/SBSA packages
include this helper and `player-model-builder`. The shared desktop supplies
`HM_HSTREAM_MODEL_PREPARE`; installed helpers are adjacent to `hstream-cli`, and
development builds use `src/apps/hstream-model-prepare/hstream-model-prepare`
under the selected Bazel output tree.

## Exact native appearance inference

Python and the native SDK may ship incompatible TensorRT serialization versions.
`hstream-model-infer` keeps the accepted engine in a persistent native process;
it never rebuilds or substitutes the model. The worker selects the matching
helper through `HM_HSTREAM_MODEL_INFER` and starts it with `--engine PATH`,
`--gpu-id N` and its direct `--parent-pid`. The same helper ships beside the other
native binaries on desktop, Jetson and ARM64/SBSA.

The Python graph and helper retain preparation's accepted tracker device across
seek rebuilds; the selected portable ONNX ReID model passes it to its CUDA
provider as well.

Requests contain a little-endian uint32 crop count from 1 through 32 followed by
float32 `[N,3,256,128]` preprocessed pixels. Responses contain that same count and
float32 `[N,256]` raw features; the model's configured output normalization remains
owned by the Python adapter. A zero count or clean stdin EOF ends the session.
Partial records, unsupported engine bindings, nonfinite values and oversized
batches fail visibly on stderr and close the transport. Stdout contains binary
responses only; `--describe` is a separate JSON inspection invocation. Requests
are split or zero-padded to the engine's exact fixed/dynamic batch profile, and
padding outputs are discarded.

The Python owner enforces read/write deadlines, validates response sizes and
stops/reaps the helper during worker teardown. The helper additionally monitors
a Linux pidfd for the owning process and sends itself SIGTERM when that process
exits, including while blocked on input or CUDA. The session survives its
creating Python worker thread exiting; `PR_SET_PDEATHSIG` would incorrectly end
it at that point. The helper creates no descendants. Its GPU buffers and
execution context are reused across requests. The ABI boundary requires a
bounded host transfer of preprocessed appearance crops: at most 12 MiB of input
and 32 KiB of output per request, never a full video frame. This is confined to
the optional exact DeepStream ReID path; it avoids loading conflicting TensorRT
runtimes into one Python process.

On an RTX 5090, 20 warm requests through the production Python transport and
native helper measured median latency of 0.93 ms for one crop, 3.34 ms for eight
and 22.83 ms for 32. These measurements start with preprocessed CPU crops; they
exclude extraction/normalization and any Python GPU-to-host crop copy. The
opt-in `//src/apps/hstream-model-infer:model_infer_gpu_test`, with
`HSTREAM_REID_ENGINE` naming an existing native engine, checks real persistent
batches, invalid records, EOF, creator-thread exit and parent-process-death
cleanup without preparing a new model.
