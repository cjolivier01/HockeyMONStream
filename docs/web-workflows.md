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
| `highlight-live-preview` | The same plan/assets/selection, plus required boolean `loop`. Plays the shared native authored reel through hardware H.264/Opus WebRTC, without an MP4 artifact. The supervisor supplies `--preview-port=N`; payloads cannot select ports. Finite completion publishes only `operation-data.json`; Stop/cancellation publishes no artifact. |
| `highlight-card-preview` | One authored card in a native schema-2 `plan`, with no `archive_path` or `selected_indices`. Uses the desktop's `RasterHighlightCard` to publish `highlight-card.png`, fitted to the selected archive's aspect inside the existing 1920×1080 raster bound. Returns `preview`, `width`, `height`, `source_width`, and `source_height`. Requires the frozen metadata probe described below. |
| `calibration-complete` | Trusted setup postprocessor after a successful bounded calibration-only CLI run. Requires nonempty `invalidation_id` matching the persisted owner and optional boolean `require_ice_mask`. Validates the native generation, completes its calibration state, and publishes `game/setup-preview.png` through the shared Qt saved-still reader (2048-pixel maximum). Prefers a current `s.png`, falling back to `panorama.tif`; a missing valid image or generation fails completion. Returns `state`, `invalidation_id`, and `preview`; the receipt retains calibration artifacts for adoption. |
| `calibration-still-read` | Read-only idle preview (`saved-calibration-still-v1`). Requires a `config` object containing frozen effective settings; optional `expected_generation` binds an exact native generation. Returns `calibration_still_version: 1`, `available`, and `retryable`. Available results also contain `preview: "saved-stitching-preview.png"`, `width`, `height`, `source_image` (`s.png` or `panorama.tif`), and `source_revision` (SHA-256 of the native stat revision). The receipt contains only this run-root PNG and `operation-data.json`; unavailable results publish metadata alone. No calibration adoption, invalidation completion, reframe or source-config write occurs. |
| `matches-read` | Optional `fingerprint`; omission reads the current native match snapshot. Optional trusted `workspace` selects an experiment instead of the main game. Returns original camera images, dimensions and endpoints. |
| `matches-publish` | Existing `fingerprint` and `frames` containing `index` and `matches` (`left`, `right`, optional `score`). Validates original-image endpoints and publishes an immutable manual snapshot. This does not activate new maps; use the candidate edit/solve/promote sequence. |
| `experiment-create` | `settings` with `control_points`, `frame_count`, `stitch_frame_time`, optional matcher/resolution/rotation/camera/filtering, plus optional `sequence`. Creates a native isolated pending candidate. |
| `experiment-edit` | Saved `workspace`, inspected `fingerprint`, replacement `frames`, and optional `sequence`. Creates a new candidate from immutable edited matches without changing the inspected candidate. |
| `experiment-prepare-edit` | Saved `workspace` (or completed main calibration) and `sequence`. Uses the desktop editable-copy primitive, then solves and returns the original-image match editor receipt under one supervisor owner. |
| `experiment-save-matches` | Saved `workspace`, expected `fingerprint`, edited `frames`, and `sequence`. Creates the desktop edited candidate and solves it before releasing ownership. Stale snapshot/generation, cancellation and solve failure publish no successful result. |
| `experiment-recover` | Worker-staged immutable failed-run settings/media and `sequence`. Captures a new private editable snapshot. A failed automatic solve may still return verified original images with zero correspondences; its result is `state: editable`, `calibration_complete: false`, never a solved calibration. |
| `experiment-run` | Saved `workspace` and optional `require_ice_mask`. Runs native calibration through `hstream-job --interactive`, then validates native completion before returning success. |
| `experiment-inspect` | Saved `workspace`. Returns versioned saved-frame inspection, exact source identities, coverage, images and source validation; see below. |
| `experiment-complete` | Saved `workspace`, optional `require_ice_mask`. Validates already-generated native calibration; it does not run a solve. |
| `experiment-promote` | Saved `workspace`. Transactionally promotes the candidate's existing native calibration into the run's main game. The web worker controls adoption into the project. |
| `track-runs` | `database` and optional `game`; omission enumerates the database's game IDs. Returns native telemetry run IDs. |
| `track-choices` | `database`, `run_id`, `route` (`program`, `stitched`, `program-full`), `game_ms`, optional `pts_offset_ms`. Returns native segment-bound track choices. |
| `track-capture` | Track choice fields plus `plan`, `archive_path`, `archive_offset_ms`, `item_index`, `cue_index`, and `choice`. Uses native track capture and returns a portable plan and `tracked-plan.json`. |
| `camera-replay` | `database`, `run_id`, bounded `start_seconds` and `duration_seconds`, native `tuning`, and optional fast/follower target booleans. Returns original, baseline and trial trajectories from `ReplaySession`. This operation does not encode a moving camera-preview video. |
| `camera-experiment` | A completed `database`/`run_id`, `media_kind` (`original-cameras` or `panorama`), bounded `start_seconds`, `duration_seconds` (0.1–120), `playback_start_seconds`, `video_origin_seconds`, and explicit source-confirmation/playback/overlay booleans. Original cameras use the worker-staged game calibration; panoramas require `archive_path` and validated media dimensions. Uses the shared `ReplaySession`, decoder/cropper and preview worker with hardware WebRTC output. The supervisor assigns `--preview-port=N`. |

Camera Experiments accept sequential typed stdin commands for play/pause, Stop,
frame seek, comparison view, overlays, repeat, real-time playback, source
confirmation, parameter/rink-aware trials, Save and Close. Native events use
`HSTREAM_CAMERA_EXPERIMENT ` and fence run, stage, generation and request. The
browser presentation receipt must match the operation and original source PTS
before a seek is ready. Stop retires the source while retaining the browser peer;
Close retires the worker before server ownership is released. A bounded client
lease sends abandoned sessions through the same supervised cleanup.

The native adapter writes a revisioned trajectory catalog separately from its
frequent state messages. Save publishes `camera-trial-N.yaml` with an exact
file-identity receipt, allowing an immediate authenticated download before Close;
final publication rechecks the same artifacts. Both interfaces resolve defaults
from `camera_experiments` in the bundled baseline and share the archived recording
source-origin calculation and immutable historical mask. Legacy manifest import
is still unavailable in the browser. Scaled-panorama mask rendering, reconnect,
abandonment and authenticated rink-aware paired trials remain
qualification gaps; the presence of this operation does not establish full
Camera workflow parity.

Workspace descriptors returned in `result.workspace` contain `root`,
`game_directory`, `game_id`, `invalidation_id`, and `settings`. Their paths are
relative to the current run. The worker must clone the native snapshot and
reconstruct authorized camera aliases when staging a subsequent operation.
Immutable match/source metadata must never be rewritten to bypass provenance
checks. Camera aliases should resolve to stable authorized upload identities;
per-run hardlink paths change the native canonical-path fingerprint.

The `match-editor-v1` feature adds `match_editor_version: 1`, `automatic`,
`automatic_compatible`, and `editor_limits` to `matches-read` and editable-copy
results. Current and automatic frames retain native image dimensions, exact
source times, opaque source-path identities, and original PNG artifact references.
The browser displays only the selected pair and edits original-camera coordinates.
Inspection reports `editable_snapshot` and `editable`; match JPEGs alone do not
establish editability. The current snapshot and workspace generation are checked
again by `CreateEditedStitchingExperimentWorkspace` before saving. Reset uses the
same input/frame compatibility checks as the desktop editor.

The web server permits recovery only from an owned terminal run after cleanup
has released it. It freezes the original settings, source order and upload IDs,
then verifies their digest again at staging. A cleaned failure is regenerated
from those uploads; no deleted intermediate is assumed to remain available.
The failed source stays failed. Recovery evidence and any subsequent read/inspect
results cannot be promoted, completed or automatically solved through the old
candidate commands; Save and recalibrate must produce a successful new candidate.
The ordinary workflow quota, time limit, process containment and cancellation
rules apply to the complete copy/capture/edit/solve operation.

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
Its camera-profile catalog merges the bundled baseline with game overrides, so
sparse game configs retain the same fallback optics and verified recording-mode
profiles as Qt. This merge never replaces the validated private playlists.
Unedited frozen sources retain their snapshot; explicit profile, optics or policy
edits refresh automatic components. Unavailable metadata uses the selected
profile's fallback. Reopening a saved descriptor never rereads MP4 metadata.
The returned workspace stores the complete camera selection, including
nullable automation policies, and both filtering choices. Subsequent workers
retain those frozen settings through inspection, solve, edit and promotion.
`experiment-inspect` returns `inspection_version: 1`, `player_selected`,
`expected_pair_count`, `anchor_ns`, `fingerprint`, `source_validation`, and
`coverage_grid` (columns, rows, and the same three band colors as Qt). Each frame
retains two positional `thumbnails` and two `match_images` (points then lines);
missing files are JSON null, so a missing left image never shifts the right image.
The two `sources` contain `available`, filename-only `name`, `source_seconds`,
`source_pts_ns` and `decoded_sequence`. Nanosecond timestamps and sequence IDs
are decimal strings to preserve 64-bit values in JavaScript. Frame `coverage`,
`size_band_counts`, `eligible_people`, `quality` and `timeline_ns` come directly
from the shared inspector. Coverage is in baseline stitched scoring cells,
not raw-camera coordinates. Inspection reads saved artifacts without launching
decoding, inference or calibration. Browser artifact IDs replace native paths
at the existing authenticated result boundary.

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

## Authored card preview

The browser submits an owned published archive UUID and the current unsaved
card fields. Admission snapshots the archive's file identity and each immutable
logo asset; it does not save the accepted reel or the modal draft. Worker staging
copies only the bounded logos and supplies exactly one `authorized_media` path,
plus `runtime.archive_probe` containing that same path and an `identity` object.
The five identity values (`dev`, `ino`, `size`, `mtime_ns`, `ctime_ns`) are decimal
strings. These paths and identities are worker-only, never browser inputs.

Native inspection opens every path component without following symbolic links
and pins the regular archive's read-only descriptor. It verifies the identity
before and after `ffprobe`, which reads `/proc/<native-pid>/fd/<descriptor>` with
the MOV demuxer, only the file protocol, and external data references disabled.
Inspection retains the existing 30-second deadline, 1 MiB combined output bound,
and cancellation handling. No multi-GB archive copy, hardlink, or symlink is
retained in the preview run. Static card rasterization is the same CPU operation
as Qt; it neither decodes archive video nor reads GPU frames back.

The web worker verifies the exact PNG/metadata artifact pair, the native raster
bounds and image dimensions before publication. The browser starts an initial
preview when the card editor opens with an archive selected, and Preview card
renders the current unsaved fields. Stop/close retire that modal's generation
and cancel its pending run, including admission that returns after close.
Late result and image-load callbacks cannot repopulate a retired preview.
The reel's Preview/Loop actions use `highlight-live-preview` for moving video and
authored cues. The item editor's held-frame inspection, in-frame positioning and
recorded-track picking remain separate work.

## Live authored reel

`HighlightWebPreview` adapts `HighlightReelPipeline` to the shared
`WebPreviewTarget` and `WebRtcSignaling`; Qt and web use the same archive decoder,
card raster, cue compositor, audio timeline and loop implementation. The target
converts only GPU-owned output surfaces and uses hardware H.264 encoding. Static
artwork remains the existing bounded CPU raster; video is never read back.

The first authored frame repeats with silence until an exact native/browser
presentation receipt is accepted. Source audio/video then start at one common
timestamp offset, so short reels cannot finish during negotiation. Browser
replacement fences receipts by peer generation. Signaling retires before graph
EOS/NULL, including cancellation without a connected peer.

The canonical `web_preview.authored_connect_timeout_ms` and
`authored_disconnect_timeout_ms` bound initial browser absence and lost current-peer
frame-batch acknowledgments. Playback still requires an exact first presentation;
after that, accepted metadata acknowledgments prove receiver liveness independently
of video-frame callbacks, which browsers may suspend in background tabs. These
acknowledgments never uncover pixels or count as presented frames. The outer
supervisor still owns descendant cleanup and keeps the
recording reserved until it confirms termination. Closing the dialog/page asks
that supervisor to cancel; a lost request cannot leave an endless loop running.

Run `scripts/test_hstream_highlight_preview.py --native-root PATH --web-root PATH
--artifacts /tmp/highlight-preview-check` with NVIDIA hardware, Chromium and
Playwright. It uses the production browser player and verifies decoded card/clip
and cue pixels, non-silent source audio, exact receipts, looping, reconnect,
finite completion, cancellation and abandoned-client cleanup. Its fixture
redirects state/ticket/signaling addresses; it does not qualify the authenticated
gateway. Compare with `highlight_reel_pipeline_test` on a GPU-backed X11 display;
Xvfb cannot provide its CUDA/OpenGL preview interop. Use Xvfb for
`highlight_item_editor_interaction_test`, and the offscreen platform for
`highlights_dialog_test`.

Add `--headed --liveness-only` on a desktop to attempt a real tab/window background
transition, suspend frame callbacks longer than the configured deadline, restore
fresh presentations, and then verify timeout when batch acknowledgments stop
despite an open peer. The result records `document.hidden`; Chromium 155 on the
current desktop keeps it false even after tab switching/minimizing, so that run
qualifies suspended callbacks, not an actual hidden-tab transition. The Qt reel
test hides its native preview while the timeline continues.

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
  //src/apps/hstream-ui:web_job_test \
  //src/apps/hstream-ui:web_card_preview_test \
  //src/apps/hstream-ui:web_card_probe_test
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

The CPU card preview test compares actual adapter pixels to direct Qt rasters
for title/matchup cards, landscape/panorama/portrait archives and PNG/JPEG/WebP
logos. The probe test covers authorization, parent/final symlinks, replacement
after opening, helper failure, output bounds, cancellation and the real timeout.

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

Interactive Program sessions that select an archive use
`hstream-job --interactive-publication`. The exporter preserves WebRTC, network
and file sinks, pacing, and inherited stdin without adding `--encode-job`.
Its frozen finalizer run ID owns the existing manifest, runtime directory and
publication transaction. Supervisor arguments, including the reserved signaling
port and private preview run ID, pass through to the CLI unchanged. The bridge
reads `work/game/runs/<native-id>/pipeline.log` while the finalizer owns stdout.
Graceful Stop drains completed files into exact native receipts; cancellation
or failed encoding cannot publish them. Preview-only sessions continue to use
`--interactive` and require no archive receipt.

The baseline's `hstream_ui.archive_routes` initializes both interfaces; the
shipped empty list permits preview-only Play. A separate
`hstream_ui.export_archive_routes` supplies the explicit file-export fallback.
Calibration may record Archive Stitched while retaining disabled Program route
choices for a later Program session. Its worker verifies both the native
archive receipt and the subsequent `calibration-complete` receipt before
adopting either output set. Shared sidecars must agree in both receipts and are
retained once.

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

### Live rink leveling and crop editing

The private `hstream-web-job --rink-editor REQUEST.json` CPU helper advertises
`rink-editor-v1`. It validates the same original camera PTO and image geometry
as Qt and runs the shared point formatting, ray parsing and corner/post
estimators. The supervisor supplies the bounded JSON and runs `pano_trafo` in
its owned child process group; the helper does not publish calibration files.
Live browser prompts expose the original left/right images and preserve
rectangle mismatch confirmation, exact preview-angle acceptance and optional
seam blending. Crop previews cover the full projection and report advisory
automatic bounds. Skip leveling and Use crop retire any pending preview before
writing the generation-bound native response. These still-image tools do not
read video frames back from the GPU.

`web_rink_editor_test` checks real Hugin transforms with nonzero camera rotation,
absolute leveling recovery, original-image bounds and escaped image references.
The web rink-editor tests also run the actual helper and Hugin tools through
the supervisor bridge when `HSTREAM_WORKFLOW_TEST_BIN` is set.

### Saved rink-map editing

`rink-map-read`, `rink-map-update` and `rink-map-save` share `RinkMapBackend`
with the desktop. Each request owns a cloned completed calibration. Updates
and saves require the exact native generation, calibration revision and saved
map digest returned by Read. Drafts contain original-image pixels; supplied
rays or matrices are rejected. The native backend recomputes every ray, fits
all enabled observations, and returns the shared feature catalog, projected
layout, detection contours, mask assistance and partial/inconsistent/ready
state. Automatic suggestions must be reviewed before a separate Save.
The canonical baseline supplies rink dimensions to both interfaces.

Only a successful `rink-map-save` receipt can become the selected calibration;
Read and Update retain drafts without replacing the original source. Empty
and partial maps can be saved, matching Qt. Playback readiness is checked
separately for a fitted saved map. `web_rink_map_test` uses actual Hugin ray
transforms and covers read/fit/save, baseline precision, stale saved-map and
calibration revisions, and rejection of browser-provided rays.

### Saved leveling and projection crop

`calibration-view-read`, `calibration-view-estimate`,
`calibration-view-landmarks`, `calibration-view-preview` and
`calibration-view-save` edit a cloned completed calibration. They use the same
camera/projection compatibility check as Qt, original-image dimensions and
native generation/revision fences. Point estimates share `RinkEditor`; saved
landmark estimates share `UpdateRinkMap` and `RinkLandmarkLeveling`, including
readonly reference marks. Previewing uses the published rotation delta and
original stills. Crop previews show the full canvas and advisory automatic
bounds; they do not change the saved crop.

Leveling Save requires a completed preview for the exact snapshot and angles.
The server freezes that receipt and checks it again at dispatch. Native Save
prepares the shared `StitchingReframeIntent` and calls `reframe_stitching`,
retaining the optimized camera alignment. Cancellation or a stale snapshot
cannot publish over the source run. The completed child is staged by the
browser; Save Preset adopts it atomically with the form, matching the desktop
accept/save boundary. Only an accepted crop's `auto_crop` control changes the
child's browser geometry snapshot.

`calibration-view-reset` stages the main window's Reset Controls in another
private descendant. Admission validates the ordinary settings form and freezes
the account defaults separately. The adapter reads default framing from the
bundled baseline plus that overlay, including null/inherited rink rotation; the
browser never supplies hidden angles or an invented full-canvas crop. It uses
`PrepareStitchingReframeIntentLocked` to retain alignment for a view change or
marks changed solve inputs for ordinary calibration. Output geometry changes
retire masks and scoreboard coordinates through the native transaction.

The `saved-calibration-reset-v1` receipt binds the staged config by SHA-256 and
states whether regeneration is pending. Reset does not render or run matching.
Save Preset adopts the receipt and matching form controls atomically; changed
defaults, selected source, inputs or geometry reject adoption. Playback alone
consumes its native reframe or solve invalidation. Its supervisor retires every
pipeline descendant before invoking `calibration-complete`; publication verifies
that exact receipt alongside any selected archives. The completed successor
becomes the selected calibration only while the saved source and preset revision
still match. Cancellation and newer settings retain the previous selection.
Pending Reset results cannot open editors that require completed images. Cancel waits for supervised cleanup
and restores the previous draft; a no-op retains the current calibration.

The dialog's `view-preview.png` remains separate from `game/setup-preview.png`.
The latter is regenerated from the final panorama after view saves and
experiment promotion, with final file identities captured after native
publication. Its bounded still decode is independent of live GPU video.

`web_saved_calibration_test` exercises installed Hugin tools, native absolute
rotation recovery, saved-view preview acceptance, preserved-alignment saves,
manual crop publication and stale snapshots. Its synthetic uniform stills
explicitly allow the native hard-seam fallback because Enblend may reject
featureless overlap; real media still require separate seam qualification.
It also verifies inherited nonzero Reset defaults, unchanged source bytes,
no-op behavior, pending reframe and changed-solver invalidation. The Qt window
test clicks Reset and Save Preset and verifies the same staged framing and
inheritance boundary.

The Highlights source description uses `highlight-archive-info` and the
`highlight-archive-info-v1` capability. Its only browser input is a recording-owned
archive artifact ID. The supervisor freezes its exact file identity; the native
adapter shares the card preview's pinned read-only descriptor and bounded
metadata-only probe (30 seconds, 1 MiB probe output). It returns measured codec,
audio presence, frame rate, duration and dimensions, publishing only checked
completion metadata. No video is copied, decoded or mapped to the CPU for this
description. Active readers retain the source's cleanup exclusion until the
supervisor retires them.

`calibration-view-rotation-stage` is an internal prelaunch operation advertised
by `calibration-view-rotation-stage-v1`. Its payload contains the private copy's
exact `source_generation` and finite `post_stitch_rotate_degrees`. It locks the
Hugin artifacts and config, requires a completed matching output/Hugin
generation, and rejects every pending live authorization. It retains alignment
and mapping bytes while writing canonical rotation and retiring the dependent
output marker, panorama snapshot, rink masks and scoreboard geometry through
the same recoverable transaction used by Qt Save Preset. The selected immutable
calibration is never modified. An unchanged rotation preserves config bytes,
including inherited null values. Its result returns `source_generation`,
`desired_generation`, `hugin_generation` and `changed`, with final artifact
identity receipts. `desired_generation` describes the source dimensions with
the requested rotation; startup owns publication of the actual output geometry
and regenerated downstream artifacts.

`calibration-controls-stage` (`calibration-controls-stage-v2`) stages ordinary
unsaved Play controls in a private copy of a completed calibration. Qt Play and
this adapter call `StitchingCalibrationControls` for the dependency stage,
selected-frame/manual-match retention, reframe capture and cleanup owner. This
operation does not reset framing or release optics merely because a form omitted
hidden saved values. The outer service must validate and freeze its form before
dispatch and supply exactly these payload fields:

```json
{
  "version": 1,
  "source": {"generation": null, "config_sha256": "64 lowercase hex characters"},
  "before_controls": {},
  "desired_controls": {},
  "defaults": {},
  "sources": []
}
```

`source.generation` is the exact nonempty saved `rink.stitched_output_generation`
string, or explicit JSON null only when that field is absent or YAML null. Saved
crop/leveling reframes can legitimately complete while retiring that rendered
output identity. Empty strings, omitted request keys and mismatched presence
fail; the caller must never synthesize a generation. Version 2 advertises this
nullable identity contract; payload/result version 1 and the existing string
contract remain compatible, so the adapter also advertises the v1 capability.

The adapter applies only changed leaves between the two control snapshots over
the actual saved private config. It verifies the exact source hash, completed
nonstale state without a reframe or pending live authorization, and mandatory
Hugin preflight identity under artifact/config locks. A saved output generation
must additionally match that Hugin identity. Existing backend claims must match
their original owner and frozen resolved controls; supported legacy completed
sources without a claim do not acquire one merely for admission. The returned
source preserves the exact nullable identity, and every accepted stage still
claims a fresh invalidation owner. The old tuple resolves from bundled baseline,
source-owned `before_controls`, then the original private config. That last layer
preserves hidden framing and saved optics. New account Defaults cannot change
the claim being authenticated. Desired controls resolve against the bundled
baseline plus the supplied current frozen calibration overlay; staging never
reloads the process user's defaults. Legacy private width aliases retain precedence over baseline
values. `defaults`, `before_controls` and `desired_controls` accept these canonical
calibration roots:

- `stitching`: `control_point_limit`, `calibration_frame_count`, `stitch_frame_time`,
  `sync_method`, `control_point_matcher`, `control_point_resolution`,
  `control_point_filtering`, `control_point_execution_provider`, `mapping_backend`,
  `camera_config`, `camera_fov`, `camera_automation`, `input_lens`,
  `auto_input_lens_frozen`, `run_autooptimizer`, `projection`,
  `projection_parameters`, `projection_framing`, `max_output_width`,
  `post_stitch_rotate_degrees`, `rink_config`, `rink_mask_frame_time`, `rink_mask_frame_time_default`,
  `blend_mode`, `blend_feather_fraction`, `dtype`.
- `rink.camera.fixed_edge_rotation_angle`, as one number or `[left, right]`.
- `hstream_ui.stitching_calibration.control_points`, `frame_count`,
  `hstream_ui.show_crop_dialog` and `hstream_ui.show_leveling_dialog`.

Map controls accept their native fields only. Projection parameter keys must be
native projection names. Arbitrary pipeline settings, paths, commands and custom
camera/rink catalogs are excluded; any required custom catalog must already be
frozen in the verified private config.

`stitching.rink_mask_frame_time` is the native mask-time control; the adapter
rejects `rink.mask_frame.time` and split canonical fixed-edge keys. Rink/time
edits restore generated mask-time bookkeeping before applying the new choice,
so an inherited old rink time does not override the newly selected rink. Each
blend edit retires only that field's aliases. Unchanged blend controls keep their
effective native values.

The supplied defaults are frozen for this staging comparison. The outer launch
must also preserve that snapshot: its effective calibration values must resolve
from the same baseline/defaults with the staged private config taking precedence.
It must disable or override mutable process-user defaults and must not merge the
entire desired form over the staged config again, which would erase hidden saved
leveling/crop values. A staging receipt alone does not prove that runner
configuration equality; the outer integration must verify it before playback.

Managed publication can reserve its archive identity before staging and export
afterward with `hstream-job --run-id ID`. The exporter and finalizer share the
same ID validation; ordinary exports still generate a fresh ID when it is
omitted. Plain `--interactive` and `--runtime-only` sessions reject this option
because they have no publication receipt. This archive identity is separate from
the calibration invalidation owner returned by the staging operation.

An empty `sources` retains the admitted playlists. A nonempty list supplies every
ordered chapter from both roles as `{id, before_role, after_role, before_alias,
after_alias}`. Roles are `left` or `right`; aliases are private relative paths.
Both aliases must exist and resolve to the same regular recording listed in the
request's `authorized_media`. Same-role entries retain the same alias. Source
changes release selected/manual input references, synchronization and the active
mask-input reference; immutable historical bundles remain intact. Automatic
optics resolve only through those validated private playlists before freezing.

Every successful stage reserves a new `invalidation_id`, including unchanged
Play. Matching/budget/provider changes restart `features`; compatible view edits
reuse alignment at `canvas`; source/time/count changes restart `input`. A retained
selected-frame plan rejects a changed reference time until its count is explicitly
changed. Native cleanup preserves synchronized inputs when the retained plan or
manual matches owns them. Cleanup completion checks the owner, pending stage and
unchanged controls/selection, allowing only the cache removals that cleanup owns.
Legacy completed frame counts remain authoritative when the canonical count is
absent and the form count is unchanged. Synchronization edits always restart
input alignment, including when the same request changes crop/projection.

The result includes `calibration_controls_stage_version: 1`, `staged`, the exact
original `source`, `invalidation_id`, `pending`, `start_stage`, `cleanup`
(`none`, `all` or `control-points`), `cleanup_completed`, `alignment_reused`,
`retained_selected_frames`, `retained_manual_matches`, `output_changed` and final
`config_sha256`, alongside the standard exact file-identity receipts. Cancellation
before publication preserves the clone; cancellation afterward leaves a private
incomplete workspace without a completion receipt. The supervisor must verify
staging, bind the launch to that owner, retire all descendants and then run
`calibration-complete`. A staging receipt alone never proves a completed solve or
authorizes changing the project's saved preset or selected calibration.

`stitching_calibration_controls_test` covers the shared dependency/retention
matrix and stale cleanup owners. `web_saved_calibration_test` adds actual native
cleanup and Hugin reframe fixtures, exact receipts, hidden leveling preservation,
source roles and cancellation around publication/cleanup. The Qt window test
exercises source replacement through its real actions. These regressions do not
replace paired Qt/authenticated-browser workflow tests against one native build;
retain the web parity limitations until those comparisons pass.

## Prepared model imports

The browser imports account-owned Pose, Jersey, Activity, Team and Appearance
assets with `hstream-prepared-model-validate --request FILE`. This CPU-only
validator shares `PreparedModelValidation` with the Qt Custom chooser. It parses
the existing prepared manifest or ReID configuration, checks readable nonempty
model files, and retains runtime SDK/GPU compatibility checks in inference.
Its `prepared-model-validation-v1` request admits at most 64 files, streams SHA256
checks, rejects symlinks and escaping paths, and publishes an exclusive
`model-validation.yaml` only after the exact input set validates. Metadata reads
are bounded to 1 MiB. The outer web supervisor owns timeout, cancellation and
descendant retirement. A dispatcher therefore needs the matching CPU validator
but does not need Qt, CUDA or a GPU allocation to validate an import.

The web registry stores UUID selections and frozen identities. Each admitted run
owns checked private copies, charged to its run allowance. A successful import
receipt proves file/manifest validation; it does not qualify an engine against a
different TensorRT release or GPU. Run the selected custom model on the intended
compute host before claiming execution parity.
