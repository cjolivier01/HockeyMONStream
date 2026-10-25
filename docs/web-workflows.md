# Headless native web workflows

`//src/apps/hstream-ui:hstream-web-job` exposes the libraries used by Highlights,
Stitching Experiments, the match editor, recorded tracks, and camera replay. It
uses Qt's offscreen platform for fonts and creates no desktop window. Run it as:

```sh
hstream-web-job --request /private/run/operation-request.json
```

The authenticated web worker owns admission, input staging, cancellation,
process containment, storage reservations, and publication. This executable is
not an HTTP service and must not accept arbitrary browser-created requests. The
worker creates a fresh private run directory, snapshots authorized inputs, and
generates the request there. A supervisor must stop all descendants before
detaching camera aliases, releasing input references, or publishing artifacts.

## Request and receipt

Workers probe `hstream-web-job --capabilities` before admitting work. This
display-independent command returns protocol `schema`, `request_schema`,
`receipt_schema`, the supported `operations`, `features`, and request/result byte limits.
The operation list also drives request validation, so readiness cannot advertise
operations the request parser rejects. HockeyMONWeb checks its required operations
against this deployed executable in addition to checking the generated startup
catalog. Deploy changes to this contract with the matching web worker.

```json
{
  "schema": 1,
  "run_id": "example-run-1234",
  "operation": "highlight-preview",
  "root": "/private/run",
  "game_dir": "game",
  "payload": {"plan": {"schema": 2, "intervals": []}}
}
```

`root` must be the request's parent directory. Payload paths are run-relative;
absolute paths, parent traversal, and escaping symlinks are rejected. Requests
are limited to 16 MiB and result documents to 32 MiB. The highlight example
above illustrates the envelope only: rendering requires a complete native
`HighlightPlan` with at least one valid item.

`operation-result.json` is published atomically after output files are synced.
Its fields are `schema`, `state`, `runner_succeeded`, `run_id`, `operation`,
`artifacts`, and, on success, `request_sha256` and `result`. Each artifact gives
`path`, `kind`, `size`, `device`, `inode`, and `mtime_ns`. Identity integers are
decimal **strings** to preserve nanosecond precision. Workers must compare every
identity field against the final regular file before publication. The result is
also retained as an `operation-data.json` metadata artifact.

Repeating the exact completed request returns successfully without modifying
its receipt. Reusing that run for different input fails and preserves its
receipt. A failed or cancelled run needs a new run directory for retry. Failure
receipts publish no artifacts. SIGTERM/SIGINT request cancellation; the outer
supervisor still owns the final descendant-stop and storage checks.

Completed-run retries read receipts using the same 32 MiB bound as publication;
the smaller 16 MiB request bound must not reject a previously successful result.

Qt/browser comparison coverage and the remaining workflow differences are tracked
in the companion HockeyMONWeb repository's `docs/qt-web-parity.md`. Changes to
desktop UI sources require regenerating and reviewing its Qt control/layout
contract, then testing both interfaces against the same native checkout.

## Operations

| Operation | Payload and behavior |
| --- | --- |
| `highlight-preview`, `highlight-render` | Native schema-2 `plan`, optional `archive_path`, `archive_offset_ms`, and `selected_indices`. The existing GPU reel pipeline encodes cards, clips, overlays and audio to `highlights.mp4`. Preview is bounded to 30 seconds; export to 24 hours. Cards alone need no archive. Asset and recorded-binding paths must be run-relative. |
| `calibration-complete` | Trusted setup postprocessor after a successful bounded calibration-only CLI run. Requires nonempty `invalidation_id` matching the persisted owner and optional boolean `require_ice_mask`. Validates the native generation, completes its calibration state, and publishes `game/setup-preview.png` from the existing panorama using the shared bounded (1920-pixel maximum) reader. Missing panorama or generation fails the operation. Returns `state`, `invalidation_id`, and `preview`; the receipt retains calibration artifacts for adoption. |
| `matches-read` | Optional `fingerprint`; omission reads the current native match snapshot. Optional trusted `workspace` selects an experiment instead of the main game. Returns original camera images, dimensions and endpoints. |
| `matches-publish` | Existing `fingerprint` and `frames` containing `index` and `matches` (`left`, `right`, optional `score`). Validates original-image endpoints and publishes an immutable manual snapshot. This does not activate new maps; use the candidate edit/solve/promote sequence. |
| `experiment-create` | `settings` with `control_points`, `frame_count`, `stitch_frame_time`, optional matcher/resolution/rotation/camera/filtering, plus optional `sequence`. Creates a native isolated pending candidate. |
| `experiment-edit` | Saved `workspace`, inspected `fingerprint`, replacement `frames`, and optional `sequence`. Creates a new candidate from immutable edited matches without changing the inspected candidate. |
| `experiment-run` | Saved `workspace` and optional `require_ice_mask`. Runs native calibration through `hstream-job --interactive`, then validates native completion before returning success. |
| `experiment-inspect` | Saved `workspace`. Returns native inspection images, timestamps, quality and source validation. |
| `experiment-complete` | Saved `workspace`, optional `require_ice_mask`. Validates already-generated native calibration; it does not run a solve. |
| `experiment-promote` | Saved `workspace`. Transactionally promotes the candidate's existing native calibration into the run's main game. The web worker controls adoption into the project. |
| `track-runs` | `database` and optional `game`; omission enumerates the database's game IDs. Returns native telemetry run IDs. |
| `track-choices` | `database`, `run_id`, `route` (`program`, `stitched`, `program-full`), `game_ms`, optional `pts_offset_ms`. Returns native segment-bound track choices. |
| `track-capture` | Track choice fields plus `plan`, `archive_path`, `archive_offset_ms`, `item_index`, `cue_index`, and `choice`. Uses native track capture and returns a portable plan and `tracked-plan.json`. |
| `camera-replay` | `database`, `run_id`, bounded `start_seconds` and `duration_seconds`, native `tuning`, and optional fast/follower target booleans. Returns original, baseline and trial trajectories from `ReplaySession`. This operation does not encode a moving camera-preview video. |

Workspace descriptors returned in `result.workspace` contain `root`,
`game_directory`, `game_id`, `invalidation_id`, and `settings`. Their paths are
relative to the current run. The worker must clone the native snapshot and
reconstruct authorized camera aliases when staging a subsequent operation.
Immutable match/source metadata must never be rewritten to bypass provenance
checks. Camera aliases should resolve to stable authorized upload identities;
per-run hardlink paths change the native canonical-path fingerprint.

Experiment settings use the desktop's canonical camera map: `camera` contains
`camera_config`, `camera_fov`, `input_lens`, and `camera_automation`.
`control_point_filtering` contains `epipolar_ransac` and
`homography_consistency`. Both accept partial overrides at creation. Omitted
values inherit the selected source config over the bundled baseline; choosing a
camera profile resets inherited FOV/lens values to that profile before applying
explicit optics. Native `GameConfig` and `InputLens` readers validate these
values and preserve legacy manual selections. After source aliases and playlists
are validated and linked into the private candidate, `ReadAutoInputLensForGame`
imports metadata for automatic components, matching the Qt experiment dialog.
Unedited frozen sources retain their snapshot; explicit profile, optics or policy
edits refresh automatic components. Unavailable metadata uses the selected
profile's fallback. Reopening a saved descriptor never rereads MP4 metadata.
The returned workspace stores the complete camera selection, including
nullable automation policies, and both filtering choices. Subsequent workers
retain those frozen settings through inspection, solve, edit and promotion.
The `experiment-optics-filtering-v1` capability is required by the web worker so
an older adapter cannot silently discard those settings. The browser offers
explicit source inheritance and displays the frozen settings for comparison.

For `experiment-create`, the worker also supplies a top-level `authorized_media`
array of absolute canonical upload paths. The native staged-workspace API
rejects every source symlink whose target is absent from that list and rejects
directory links. Storage objects may have opaque UUID filenames; their trusted
local camera aliases retain the video extension. Ordinary desktop workspace
creation continues to require sources inside its selected game.

Calibration operations retain nonempty native files in the main game and
experiment directories, excluding media aliases and lock files. These files
include the native provenance needed for a later inspection or promotion.

## Trusted runtime for candidate solves

`experiment-run` additionally requires a top-level `runtime` object containing
absolute operator-configured `exporter`, `cli`, `root` (working directory), and
`config` (pipeline YAML) paths. The browser must never supply these values. The
adapter saves bounded calibration-only arguments into the private candidate,
uses a private runtime cache and output directory, and forwards logs to the
outer supervisor. `--interactive` preserves the candidate invalidation
environment and leaves cleanup and publication with the web worker.

## Validation and platforms

```sh
bazelisk test --config=opt --cpu=k8 \
  //src/apps/hstream-ui:web_job_spec_test \
  //src/apps/hstream-ui:web_job_test
python src/apps/hstream-ui/WebJobTest.py \
  bazel-bin/src/apps/hstream-ui/hstream-web-job --gpu
```

The CPU tests cover confinement, exact identities, invalid request failure,
telemetry game discovery, native experiment creation, and failed-runner
handling. The opt-in GPU test additionally checks real card and archive
encoding, audio duration, receipt idempotency, conflicting reuse, and
cancellation. Native renderer tests require a working NVIDIA/GStreamer stack.
The target follows the existing Qt desktop-library platform policy: x86 and
native ARM64/SBSA are supported build configurations; Jetson is excluded by
`QT_UI_COMPATIBILITY`. The CLI/exporter continue to have their own Jetson path.

Desktop Debian builds install the adapter at `/opt/hstream/bin/hstream-web-job`
and provide `/usr/bin/hstream-web-job`. They include the three existing INT8
preparation helpers under `/opt/hstream/scripts`. INT8 preparation requires the
worker's optional ONNX, ONNX Runtime, NumPy and OpenCV Python environment; these
Python modules are not installed by the native binary package. Playback and
calibration remain native.

## Calibration prompts during managed renders

Export with `hstream-job --calibration-prompts` when the supervisor services
calibration requests during a managed render. This preserves explicitly supplied
`HSTREAM_RINK_LEVELING_FLOW` and `HSTREAM_PROJECTION_CROP_FLOW` values at execution
time; it does not enable either flow by itself. Managed jobs still clear stale
pending calibration, start-stage and invalidation identity variables. Without
this flag managed exports clear both flow variables, preserving unattended
behavior. The managed finalizer and publication protocol remain active.

### Validation on 2026-10-15

After rebasing onto `cfca9029`, the x86 optimized CLI build passed, along with
`job_script_test`, `web_job_spec_test`, `web_job_test`, `publication_test`,
`cleanup_test`, and `remux_stdin_test`. The libsoup repository unit suite passed
six tests. Bazel ran in `--batch` mode because the restricted environment could
not start its server socket. Local validation supplied matching extracted
libsoup3 headers and a local checkout of the exact pinned jetson-utils revision
with the committed compatibility patch applied; production builds obtain that
same revision and patch through WORKSPACE.

ARM64 and Jetson build attempts could not fetch `rules_cuda` because outbound
DNS was unavailable. The validation host is x86; these attempts do not establish
ARM64 or Jetson compatibility. No GPU processing was launched outside Slurm.
A complete stitched render, live browser media, and deployment on the intended
Slurm target remain required acceptance checks.

### Setup-first completion

Before starting a fresh calibration-only CLI, the worker writes a pending
`hstream_ui.stitching_calibration` owner with `invalidation_id`, positive
`control_points` (native default 1500), `frame_count` (1–64), `status: pending`,
`rink_mask_status: pending`, `stale_from: features`, and
`artifacts_invalidated: false`. Fresh runs remove inherited `reframe` and
`backend_generation`. The worker sets `HSTREAM_CALIBRATION_PENDING=1`,
`HSTREAM_CALIBRATION_START_STAGE=features`,
`HSTREAM_STITCH_CALIBRATION_INSPECTION=1` (retains editable control-point snapshots), and
`HSTREAM_CALIBRATION_INVALIDATION_ID` to that owner, passing the matching
`--clean-expected-invalidation-id`. Use `--stitching-calibration-only`, a FAKE
sink, and a bounded duration such as `-t=1`; this does not launch Program
inference. The forced recipe must follow user overrides.

After clean CLI exit and descendant cleanup, `calibration-complete` performs
the native completion validation. Only its successful, fresh identity receipt
allows the web worker to publish and adopt calibration. The preview is a
single bounded decode of the saved panorama; no video frame is read back from
the GPU. The outer worker must retain its Slurm reservation, timeout, disk
quota, and cancellation enforcement through this postprocessing step.
