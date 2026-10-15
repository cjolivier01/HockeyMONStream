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

## Operations

| Operation | Payload and behavior |
| --- | --- |
| `highlight-preview`, `highlight-render` | Native schema-2 `plan`, optional `archive_path`, `archive_offset_ms`, and `selected_indices`. The existing GPU reel pipeline encodes cards, clips, overlays and audio to `highlights.mp4`. Preview is bounded to 30 seconds; export to 24 hours. Cards alone need no archive. Asset and recorded-binding paths must be run-relative. |
| `matches-read` | Optional `fingerprint`; omission reads the current native match snapshot. Optional trusted `workspace` selects an experiment instead of the main game. Returns original camera images, dimensions and endpoints. |
| `matches-publish` | Existing `fingerprint` and `frames` containing `index` and `matches` (`left`, `right`, optional `score`). Validates original-image endpoints and publishes an immutable manual snapshot. This does not activate new maps; use the candidate edit/solve/promote sequence. |
| `experiment-create` | `settings` with `control_points`, `frame_count`, `stitch_frame_time`, optional matcher/resolution/rotation, plus optional `sequence`. Creates a native isolated pending candidate. |
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
