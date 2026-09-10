# Camera experiments

Status: accepted, revision 2. Both independent reviewers approved with no required design changes.

## User contract

Select a completed DriveGPT recording and a short video range (normally 10–30 seconds), change camera/play-tracker controls, and repeatedly preview the same observations from the same historical state. Each trial restores state immediately before its first processed frame and applies parameter overrides only from that boundary onward. The original recorded camera remains available as a comparison. Existing Program controls and recordings are independent of an experiment.

## Architecture

1. A native HockeyMON snapshot/restore API captures all mutable play-tracker state: all living boxes, translation and resizing state, detector player histories, counters, runtime configuration/overrides, and necessary derived values. Restoring rebinds internal pointers rather than copying addresses. Versioned persisted checkpoints use explicit fields and lossless numeric representation.
2. An HStream replay library reads the completed telemetry manifest, frame index, tracked players, both camera trajectories, configuration artifacts/events, and checkpoints. It validates identities and geometry, preserves frame order/empty samples, and maps sample IDs to media time. CSV sample IDs are not physical video frame numbers. It runs the same native forward and runtime-tuning functions as production.
3. Existing recordings can reconstruct historical state by replaying recorded tracks from the actual initialization boundary with the original configuration/events. Replay compares the resulting fast/follower camera outputs to the recording before claiming faithful reconstruction. Missing provenance or mismatched output is an explicit failure with an actionable explanation, not an invisible reset to default. The reconstructed starting checkpoint is cached for all trials.
4. New telemetry captures include periodic and boundary state checkpoints plus the arena/canvas and source timeline provenance needed to resume. Preserve HM-compatible CSV formats. Bundle manifests/config/checkpoint artifacts with published CSVs, preserving publication atomicity and generation mapping.
5. A replay session computes and caches original and candidate camera trajectories over a selected range. It owns trial parameter sets and native starting state independently of any GStreamer seek or decoder lifecycle. Mutation is transactional: one complete parameter set applies at one declared boundary. Recorded events after the fork do not silently overwrite trial controls.
6. A dedicated Experiment UI in hstream-ui selects the recording, source video/uncropped panorama and range; reuses the camera tuning definitions; provides Apply & replay, repeat, transport/frame stepping, original/trial A/B selection, and camera-box diagnostics. Background work is cancellable or generation-guarded, and teardown cannot access destroyed UI state.
7. Preview reuses native GPU decode/crop/render, pairing camera trajectory rows to video by validated frame/time identity. Original raw camera inputs or an uncropped panorama are required; a cropped Program archive cannot provide alternate views. Avoid CPU pixel readback and avoid keeping an entire uncompressed high-resolution clip in VRAM. A reusable clip decoder/preview pipeline is preferable to rebuilding the full production graph per loop.

## State and timeline details

- Camera CSV rows describe state after that sample's forward step; restore after the predecessor and process the first selected sample once.
- Snapshot includes the wrapper has-received-tracks flag, arena, frame-calculation cadence/counter, and previous results where needed. Preserve track ordering and original cadence, including empty frames. Config events and genuine seeks have different reset semantics.
- Snapshot compatibility includes schema, native implementation/config identity, live-box topology, and canvas/arena geometry. Reject incompatible state without partially mutating the active session.
- No warm-up duration or adjacent-box velocity estimate is advertised as exact state restoration. Legacy approximations, if exposed, require an explicit mode and visible label.
- Playback speed never changes policy tick rate. Every loop restores the immutable starting checkpoint. In/out bounds are defined by actual samples/PTS, with an exclusive out boundary.
- Experiment results live separately from original training telemetry. Save trial parameters and source identity with results so comparisons are reproducible.

## Validation and delivery

- Snapshot round trip after nonzero translation/zoom velocity, active braking, frozen state, and player history; continuation matches uninterrupted native execution. Test bad-state rejection.
- Legacy reconstruction parity; empty frames, sample gaps, config events, nonzero run starts, row-before/after boundary, and malformed/missing artifacts.
- Repeated trial determinism, baseline parity, parameter differences, no leakage between trials, cancellation/teardown, frame pairing, range/loop endpoint behavior.
- UI integration exercises actual Experiment controls and produces screenshots for the PR. A real recording should demonstrate alternate camera motion using the production crop path.
- Validate x86_64 and ARM64/Jetson builds before PRs; stubby is available. Ready-for-review PRs target master in cjolivier01/HockeyMON and cjolivier01/HockeyMONStream. Two independent reviews per round, fix required findings, repeat until both reviewers report no required changes. Record validation/review outcomes and screenshots.

## Design review questions

- Identify the smallest coherent implementation that fully satisfies the user contract without duplicating the native camera policy or confusing approximate and restored history.
- Decide concrete boundaries/API between native snapshot, metadata replay, pipeline preview, and UI. Identify replay provenance absent from existing exports, and prescribe explicit handling.
- Review deployment/build coupling and test feasibility on x86_64 and Jetson. Suggest corrections before implementation begins.

## Revision 2 decisions

- Scope is one continuous source/seek/reset/geometry segment per experiment; reject cross-segment ranges. In/out refer to actual sample boundaries and out is exclusive. Runtime tuning events are replayed in order during history reconstruction; a trial resolves the configuration at its fork and keeps that configuration plus trial overrides throughout its selected range. The original CSV remains a separate reference, especially if later recorded tuning events exist.
- New captures add a versioned replay sidecar containing exact ordered native TLBR input floats, tick/cadence, source and sample identity, arena and full resolved native configuration, and periodic/boundary snapshots. The existing HM CSV formats remain unchanged. Native snapshots include typed configuration and state and a bounded/versioned explicit serializer; no raw object or pointer dumps. Float representation must round-trip exactly.
- New checkpoint restoration uses a factory constructing a fresh tracker with internal pointers rebound. Legacy reconstruction is described as trajectory-verified, not bit-exact. It requires archived or explicitly supplied arena geometry, original config artifacts and implementation-compatible defaults, and bounded measured agreement against both original trajectories. Missing prerequisites fail with a helpful explanation. No current game config is implicitly substituted for missing historical geometry.
- Fix the discovered native parallel unordered_map insertion during cluster outlier collection. Preserve input order, define same-build/platform determinism, and test repeats; do not promise cross-platform bit identity without evidence.
- Preview v1 accepts a continuous uncropped panorama file with an explicit mapping: the selected telemetry origin PTS corresponds to a selected video origin PTS. Record file identity, dimensions, mapping, and geometry in the experiment descriptor. Manually bound legacy media is labeled as such; it is not claimed to be automatically verified. Different source content or changed stitching cannot be inferred safe from resolution alone. The media can be an existing Stitched archive or a panorama prepared from the game's original sources using the current production stitching path; alternate crops cannot use a Program archive.
- UI is a separate ExperimentDialog opened from HStreamWindow. Shared camera control definitions/value conversion may be factored, but mutable live Program controls and presets are never shared. Use camera-motion, braking, zoom, player-filter and compatible camera-policy geometry controls; do not treat stitch/projection changes as metadata-only trials.
- Preview is an independent small GStreamer graph using the existing GPU cropper and hmgpupreviewsink with bounded NVMM RGBA conversion, sync=true, and a native X11 target. Reuse the graph for accurate range seeks/loops. A matching-frame acknowledgement is required for frame stepping; timers only poll progress. Fence renderer before destroying its target. Screenshots use explicit one-shot presented-frame capture, never steady-state readback.
- Metadata indexing streams input with bounded parsing and retains only the chosen segment/range plus bounded native state. Preparation runs off the Qt thread and honors cancellation/generation. A failed trial leaves the previously ready trial available. Save a reproducible trial descriptor and expose original/recomputed baseline/candidate distinctions.
- Delivery responsibilities: native state and determinism in HockeyMON; metadata replay library in HStream; production capture/publication integration; separate UI/GPU preview. Design acceptance from both reviewers precedes implementation. Jetson validates portable/native/backend code; the existing Qt/GPU preview is an x86_64/X11 feature and this platform boundary remains explicit.

## Review outcome

Round 1 found missing exact inputs/geometry, configuration expansion provenance, timeline/media identity, isolated UI ownership, and native clustering concurrency/allocation issues. Revision 2 incorporates those corrections. Round 2: both reviewers approve, no required changes remain. Implementation follows the accepted boundaries above.
