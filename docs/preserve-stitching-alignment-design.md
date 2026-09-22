# Preserve solved alignment when editing the stitched view

Status: implemented follow-up to persistent experiment/frame selection. Validation is summarized below; the PR records review iterations.
Previously, leveling/crop invalidation retained exact player PNGs but cleaned control points and reran matching
and optimization. This could change an accepted experiment's alignment. The shared `StitchingReframe` contract,
`reframe_stitching` in `ConfigureStitching.cpp`, and `HuginProject::Reframe` now preserve that solved alignment.

## Required behavior

- A promoted experiment's complete result remains authoritative until a requested replacement commits.
- Editing leveling, projection, output FOV/crop, or output size reuses the exact optimized camera alignment and
  control points. No matching/player-selection inference, new frame selection, `pto_gen`, or `autooptimiser` may
  run for that operation. Downstream rink-mask segmentation remains allowed.
- Cancel/retry retains the original published generation and the requested view settings. Retrying regenerates
  only projection/maps/seam/panorama and dependent rink data. Already accepted angles/crop are not prompted again.
- Failed validation or missing required inputs is an error; it cannot silently choose a full calibration.
- Matcher, matcher resolution/provider, control-point budget, frame count, camera calibration/FOV, optimizer,
  source pairing/synchronization, and selected-input changes require a new solve. Existing input/features
  invalidation cannot be downgraded by a later geometry-only save.
- A failed preset load must not let partially reset controls overwrite the saved selection or calibration.
- Promoting an ordinary same-count historical baseline must not silently remove Main's player selection.

## Reframe intent and ownership

Add a bounded, versioned `hstream_ui.stitching_calibration.reframe` record in the game's configuration. This is a request to transform a
specific completed solve, not a second cache or an inferred `stale_from` hint. A shared stitching helper owns
serialization, validation, and comparison; UI, CLI, and native worker use the same contract.

Capture it under artifact -> config locks before editing a complete calibration. Bind the source artifact
generation, original completed alignment settings, original projection framing, representative PNG hashes,
selected-frame fingerprint/diagnostics, and the desired invalidation owner. Version-10 provenance does not
record control-point budget/count; the pre-edit completed config supplies those values. Verify that its recorded
backend generation agrees with the published artifacts before adopting it. Never adopt an already pending
input/features invalidation as a completed source.

An unchanged Play reserves a new runtime `invalidation_id` before its runner starts. If launch fails, the
calibration remains complete while its `backend_generation.invalidation_id` still identifies the original
solve. Capture binds `source_owner` to that completed backend claim and validates its full tuple against the
pre-edit configuration and published provenance. The runtime reservation cannot redefine the solved inputs;
pending input/features states remain ineligible even when an older completed backend claim is present.

Both source and requested backend must be NONA with autooptimization enabled. An OpenCV project's PTO does not
encode its solved mapping and cannot serve as this operation's source.

Subsequent geometry edits retain the same original source and update the desired owner/settings. Compare all
solve-affecting inputs against that source on each save/launch. A conflicting explicit solve edit removes the
reframe request and follows the ordinary full-solve path while preserving selected frame inputs. Invalid or
unverifiable intent fails before cleanup. Keep the original solve generation through cancellation.

Successful experiment promotion replaces the whole calibration record and explicitly clears any source or
destination reframe intent; a promoted candidate must be complete without an active request. Explicit full
clean/recalibration and authorized count/forget changes cancel intent atomically under the same locks before
removing its source artifacts. Unrelated saves and ordinary cancel/retry preserve it. No successful full solve
or reframe publication may leave a superseded request behind.
Fresh candidate/bootstrap configurations also clear inherited intent before establishing their own owner.

The intent includes stable hashes for the representative camera PNGs because the current artifact generation
fingerprint does not include those files. With player inputs, validate the representative index and image hashes
against the retained bundle. Reframing reads the published stills/PTO; original recordings need not be decoded.

## Native operation

Add `HuginProject::Reframe` within the existing owned staging/publication path. Stage the exact representative
stills, unchanged `hm_project.pto`, and published optimized `autooptimiser_out.pto`. The optimized PTO is the
alignment source; `hm_project.pto` is pre-optimization and is retained as provenance only.

Use `RinkLevelingRotationDelta(published_rotation, {0,0,0})` to remove only the previous shared view rotation from
a private PTO. This preserves the optimizer's own leveling and both cameras' relative orientation. Apply the
requested projection/framing through the existing `ProjectionPanoModifyArguments`, then share the existing
canvas constraints, NONA, full-quality enblend, panorama, validation, and publication code. Preserve all
matching/representative diagnostics. No new leveling/crop handshake runs for saved accepted geometry.

The initial legacy path requires desired `auto_canvas=true` (the current default and the affected game).
Version-10 final PTO may already be scaled; its remap extents cannot reconstruct the original optimized canvas
for `auto_canvas=false`. Reject that case explicitly while preserving the existing result. Do not guess sizes
or silently rerun alignment. A future persistent neutral PTO would require version-aware artifact manifests and
transaction updates and is outside this bounded repair.

Validate unchanged lens/intrinsic parameters and control points plus unchanged relative camera orientation
(within Hugin serialization tolerance). Immediately before publication revalidate the original generation,
PNG hashes, current intent/owner, desired settings, and selected-input identity under normal locks.

Publish the replacement artifacts and matching config through the existing config-aware recoverable
transaction. In that same commit set calibration status to `complete` under the new owner, rink-mask status to
`pending`, mark cleanup satisfied, and clear reframe intent and `stale_from`. The existing AWAITING_CONFIG recovery
recognizes the complete calibration state; dependent rink work does not make the committed solve pending again.
Owned full calibrations also commit completed solve state with their artifacts; pending rink work never demotes a completed solve. This prevents a crash after publication from triggering a destructive retry. The old maps, seam, panorama and PTO stay published until
replacement succeeds. Before commit, cancellation removes only private staging; during commit, finish or
recover the transaction and report the actual outcome. Dependent rink/output geometry is invalidated normally.
Commit the accepted crop-review marker bound to the new PTO in the same configuration so the next Play cannot
mistake the approved view for an unreviewed crop and launch another calibration.

## Entry points and invalidation

`HStreamWindow::savePreset` captures intent before geometry-only changes are saved. `prepareStitchingCalibrationRun`
captures/preserves the same intent for Play and retry, and skips cleanup only when shared validation authorizes
reframing. Merge pending invalidation stages conservatively rather than overwriting an earlier input/features
stage with canvas. Handle explicit full clean/recalibration separately from retrying pending geometry.

`Configurator` must honor validated reframe intent before its pending-generation auto-clean; skipping only UI
cleanup would still lose the solved PTO at CLI startup. Preserve intent through backend ownership claims.
`setup_stitcher_and_masks` must recognize it before matcher-model requirements so absent matcher assets cannot
block an operation that never runs the matcher.

`ConfigureStitching` and `hmstitcher` dispatch surface-independent reframing before selected-frame replay/capture,
using the existing calibration worker, cancellation, output sizing, and map reload. A failed explicit reframe
request returns its error and never falls through to matcher initialization.
Dispatch also precedes `ensure_stitcher`, seam repair, and canvas hints that could otherwise inspect/load the old
generation under the new requested settings.

The UI reports that it is reusing the saved alignment and generating the edited view. Frame inspection remains
bound to the original selection, and retained experiment workspaces remain unchanged.

## Verification

1. Use a real version-10 promoted player experiment. Change angles/crop; make matcher, `pto_gen`, and
   `autooptimiser` unavailable/fail if invoked. Reframe must succeed with unchanged control points, intrinsics,
   relative camera orientation, selected plan, PNG hashes, and representative diagnostics.
2. Cancel each tool phase and retry/close/reopen/Play. Original solved artifact hashes survive every failure;
   retries invoke only the downstream tools. Accepted angles/crop are not requested again.
3. Inject config/artifact publication failures and exercise recovery. The visible generation is wholly original
   or wholly replaced, with no stale reframe request that authorizes an unrelated source.
4. Reject replaced source artifacts/stills, changed ownership, missing/corrupt bundles, unsupported legacy fixed
   canvas, and malformed intent before cleanup or matching.
5. Input/features edits followed by geometry saves must still require the intended new solve; count changes
   retain their existing explicit selected-frame semantics. Failed unrelated YAML loading cannot default the
   count and discard Main's selection on Save/Play/Clean.
6. Existing ordinary/full calibration, experiment promotion, restart and cached-input tests remain valid.
   Promote over a pending reframe and explicitly clean a pending reframe; neither may leave a stale request.
   Validate full x86/Jetson builds and real mini recovery/reframe on disposable game data; leave live user runs alone.

Update AGENTS.md plus leveling/crop and experiment documentation with implemented owners and limits. Review
this design and the implementation with two independent xhigh agents before completing the PR update.

## Validation results

- Full x86 and Jetson builds pass. Native ARM64/SBSA was unavailable; Jetson excludes the desktop Qt targets.
- Nine x86 suites cover the reframe contract, native publication/recovery, configuration and field-mask persistence,
  CLI startup, experiment promotion, one-pass runtime sizing, and the complete desktop UI workflow.
  The reframe contract test also passes on Jetson.
- The UI fixture uses valid completed NONA artifacts to exercise Save, Play with a fresh owner, and cancellation.
  It verifies unchanged source identity, no cleanup or repeated geometry prompts, and a retained retry request.
  Regressions also cover an unstarted runner's reserved owner and every supported projection with no parameters.
- A real disposable copy of the retained Players 6 calibration on mini successfully changed output width and
  pitch with `pto_gen` and `autooptimiser` configured to fail if called. Canceling during NONA preserved every
  published artifact hash; retry through the normal runner completed playback. Native validation confirmed
  unchanged control points, intrinsics, relative camera alignment, selected plan, and representative PNGs.
- The affected Main game was restored from its original retained experiment without matching or optimization;
  its original projects, maps, seam, panorama, representative stills, and stitching settings were verified.
