# Persistent stitching frame inputs and experiment sessions

Status: implemented by `PlayerFrameInputStore`, `StitchingExperimentStore`, and the experiment dialog/backend.
This document records the persistence, ownership, and failure contracts; platform and GPU validation remains
part of the normal build/review cycle.
## Storage responsibilities

`StitchingExperimentDialog` retains private candidate games, queued settings, completed calibrations, preview
output, and runner logs beneath the selected game. The catalog preserves every successfully queued row across
reopen, including dependencies between sessions. `ConfigureStitching` materializes the complete selected PNG
set before model creation, and `HuginProject` copies that immutable bundle with the promoted plan.
Queue preparation stops at its first failure; dependent rows require durably saved workspace keys, and reopening
rejects missing dependency records instead of treating a Players row as an ordinary calibration.
Each queued selection fingerprint describes the configuration actually copied into its private workspace, even
if Main changes while asynchronous preparation is underway. If Add already observed a selected set, its
replacement or disappearance before copying fails preparation instead of downgrading the request.

The published `left.png`/`right.png` still represent the accepted solve's reference pair; they do not replace the
complete multi-frame bundle. Ordinary calibration inspection uses its bounded manifest and JPEGs, which are
also copied on promotion.
## Required behavior

- Reopening experiments offers the current main calibration for inspection without running a baseline or scan.
- A selected Players set retains all full-resolution extracted pairs, metadata, and bounded inspection images.
  Later solves use those same input images without rediscovering people, seeking to their timestamps or
  re-extracting them. The existing one-pass runner may still decode an initial startup batch; solving does not
  move to a new pre-PLAYING owner.
- Completed ordinary candidates remain inspectable and previewable from their retained private workspace.
  This does not promise an immutable ordinary selection for a new baseline solve.
- Previously used Players counts remain available while their cache exists. Returning to a cached count restores
  its plan and input bundle. Changing its reference time is a conflict, not an implicit new selection.
- Configuration changes that affect matching or geometry may rerun the solve, but do not replace a same-count
  plan. Incompatible source bindings and corrupt existing cache entries fail explicitly.
- Retained rows provide frozen inputs without suppressing an intentional rerun of the same matrix after reopening.
  Repeated Add requests in one dialog are deduplicated while the source game configuration remains unchanged.
- Queued solves retain their frozen inputs when the main selection changes before completion. Their results remain
  durable and reusable for their count, including before the solve starts. A superseded main selection cannot
  replace a newer same-count default, reservation, or queued main snapshot when the old solve finishes.
- An unsuccessful solve does not discard a successfully frozen frame set. An unconfirmed running process group
  cannot lend its workspace to another solve.

## Storage boundaries

### Immutable selected input bundles

The stitching library owns `player-frame-inputs/<selection-fingerprint>/` inside a candidate or main game. A
versioned bounded manifest identifies the exact plan fingerprint, ordered source identities/timestamps, image
dimensions and depth, complete pair count, and content digests for all full PNGs and inspection JPEGs. Filenames
are fixed relative names selected by the implementation, never arbitrary paths from a manifest.

The library publishes a bundle from the full PNGs already required by matching, before creating the matcher or
loading its model, so a model-initialization failure cannot discard the captured inputs.
Publishing uses a private sibling staging directory, validates every pair, fsyncs data and manifest, and renames
the complete directory into place. A published bundle is immutable. An existing identical bundle is reusable;
an existing inconsistent bundle is an error. Partial staging is never advertised as a complete bundle.

The `PlayerFrameInputStore` helper owns bounded load/validation, publication and copying:

```cpp
struct PlayerFrameInputSet {
  std::vector<std::array<std::filesystem::path, 2>> images;
  std::vector<std::array<std::filesystem::path, 2>> thumbnails;
};
StatusOr<std::optional<PlayerFrameInputSet>> LoadPlayerFrameInputs(game_dir, plan);
Status PublishPlayerFrameInputs(game_dir, plan, images);
Status CopyPlayerFrameInputs(source_game, destination_game, plan);
```

The on-disk manifest is `frames.yaml`; filenames are `left_N.png`, `right_N.png`, `left_N.jpg` and `right_N.jpg`.
Load returns no value only for an absent bundle; a present invalid bundle is an error. Copy reports NotFound for
absence so its caller must deliberately choose legacy handling. Calibration reuse and publication perform full
PNG content-digest validation. Inspection should use a separate lightweight validation mode/API that verifies
the bounded manifest and thumbnails, without synchronously hashing gigabytes of full PNGs on the UI thread;
inspection is not a substitute for the full validation required before a solve.

Candidate
construction copies or reflinks the referenced complete bundle from its selection owner. The plugin validates
the current plan/source context and bundle before supplying cached CPU image paths through
`CalibrationFramePair`; `create_control_points` consumes those paths instead of capturing/downloading frames.
Resolve a valid cache hit before enabling the exact replay selector, satisfy input readiness for both capture
paths, and skip selector/EOS capture finalization. Existing worker, cancellation and restart/rewind ownership
remain in place. Store filesystem helpers do not recursively acquire artifact/config locks held by their callers;
callers enforce the existing artifact-before-config order and generation checks.
Generation claims and canvas provenance retain the existing plan fingerprint. A legacy plan with no bundle may
perform its existing exact replay once and publish a bundle: it captures the already fixed timestamps and never
reruns a baseline or searches for people. A present but broken bundle never falls back to re-extraction.
Callers persist a versioned "bundle materialized" reference beside the active plan to distinguish a never-cached
legacy plan from a previously referenced bundle that was deleted. This bounded reference carries the fingerprint
without changing the immutable plan itself: `stitching.calibration_frame_inputs_fingerprint`. The bundle manifest
carries the format version. A missing referenced bundle must fail. Publish the marker only after durable bundle
publication, under the matching config generation lock. Copy the marker with its matching plan; clear it when an
explicit different count clears that plan.
Ordinary steady-state Program video gains no frame readback.

### Persistent experiment workspaces

The UI/backend owns a persistent store inside the canonical game directory:

```text
<game>/.stitching-experiments.lock
<game>/stitching-experiments/
  owner.yaml
  index.yaml
  sessions/<session-id>/<candidate-game-id>/...
  sessions/<session-id>/runner-output/...
```

The game directory determines ownership; working-output overrides do not relocate experiments. `owner.yaml`
records the version and full canonical game directory, which must match on load. Private directories and owned
regular manifests must not resolve through arbitrary symlinks. The stable lock sits outside the removable store,
so concurrent open/discard operations cannot accidentally acquire different lock inodes.
Queue preparation fsyncs the exclusive configuration file, owned media-link directory hierarchy, and session
ancestor links before catalog publication. Directory traversal never follows or syncs source-media targets.
`index.yaml` is a bounded, atomically replaced catalog, protected by the stable game-level experiment lock. It references safe relative
session/candidate identifiers, workspace invalidation IDs, and selected count records containing the plan
fingerprint and numeric decode anchor. Candidate configuration remains the owner of actual solve settings and
the full plan; the index does not copy entire configuration histories. Image bundles remain independently
verifiable if an index write is interrupted.

The store lock covers reading the latest index, reserving a count's selection owner, merging updates and atomic
publication. A second dialog cannot reserve another live owner for the same count or replace newer catalog state
with its stale in-memory copy. Persist process-token/phase intent before launching a runner. Catalog capacity
limits are explicit errors before adding a session/row; never truncate history or evict frame sets to satisfy a
byte or entry bound. A failed catalog update is surfaced to the user rather than reported as durable success.
Starting an older queued search rechecks retained count selections under the reservation lock. If another dialog
has since saved those frames, Start requires reopening to reuse them instead of creating a new search reservation.

Each saved candidate state distinguishes queued, running, frozen-selection, complete, failed and quarantined.
On restart, a persisted `running` state is never assumed successful. The dialog checks process ownership before
allowing inspection/reuse; uncertain live ownership is unavailable. Initial row listing reads bounded metadata;
full artifact-generation validation runs on a worker before preview or promotion. It does not
automatically signal processes or restart old jobs. A stopped failed candidate may still own a valid frozen
selection and complete bundle.
Before releasing a stopped selection owner's reservation, reopening checks its bounded owned configuration for
a plan published before a crash or failed catalog write. A valid frozen plan is recovered into the catalog;
invalid or uncertain metadata keeps that count unavailable. Cancelling before a runner starts releases an unused
reservation so the same dialog can retry.
Confirmed-dead process ownership is reconciled even when selection recovery fails, so explicit discard remains
available for corrupt data. Unconfirmed processes continue to prevent discard.
Persist the existing process-session ID and unpredictable process token, not only a PID or generation counter.
PID reuse cannot prove an old owner stopped; an owner whose identity cannot be established remains quarantined.

No automatic eviction or age-based deletion is introduced. The dialog shows the storage location. Closing,
successful promotion, or creating a new batch does not delete retained data. **Discard experiments…** explicitly
confirms removal of all owned history, per-count selections, candidate bundles/configs, output, and logs after
workers have stopped. It preserves the main game's promoted maps, config, and frame bundle. Individual queued
rows can be removed with their queued dependents. Manual disk cleanup is also supported while dialogs and
runners are closed.

A durable `discard_pending` owner marker makes interrupted whole-store deletion retryable. Reopening such a
store reports the incomplete discard and enables only the explicit discard retry; it never deletes automatically.
A queued-row removal commits the catalog first and reports any later filesystem cleanup failure, allowing the
UI to reload the committed catalog instead of presenting already-removed rows.
## Choosing a frame set

Resolve inputs in this order:

1. A valid same-count plan in the main game is authoritative. Its exact fingerprint and reference spelling or
   absence are preserved. A changed main plan supersedes a historical default for that count.
2. Otherwise, the per-count cache supplies its previously frozen plan and complete bundle. Validate canonical
   game ownership, physical chapter identities/order, synchronization and numeric reference before reuse.
   If no default or reservation exists, the most recently queued immutable main snapshot supplies the retained
   set for that count. Runner completion order does not reorder these snapshots.
3. Otherwise, a newly requested player selection creates one baseline and one selection owner. An unchecked
   fresh count creates an ordinary candidate. The first successful plan becomes the cache entry for that count.

A present same-count main plan with invalid source bindings or an invalid/missing referenced bundle is an error;
resolution cannot fall through to history or a new scan.

Matcher, control-point budget, projection, camera geometry and rink rotation are solve settings, not selection
cache keys. Source identity, chapter order, synchronization and anchor are selection inputs. Camera geometry
remains part of the original scoring provenance but may change for a later solve, as existing replay validation
already permits. A count entry with incompatible source inputs reports the conflict; it is not silently replaced.

The index may preserve older session records, but a count has one active selection for ordinary Add options
behavior. An explicit historical selection, if exposed, chooses a specific fingerprint rather than depending on
directory iteration order. Missing index entries may be rebuilt only from valid owned manifests; scanning a cache
directory must not itself run calibration or inference.

## Reopened dialog and main inspection

Reopening loads retained rows and adds a read-only `Main calibration` row bound to the current main config and
published generation. This row is inspectable immediately when its bundle or ordinary manifest is present.
Unavailable legacy inspection data is reported honestly; merely opening the inspector never generates it.
Main inspection does not require available source recordings. Re-solving cached Players inputs still enforces
the source-binding policy above.

Finished retained rows must not prevent appending new queued options. Start runs only queued new candidates,
while existing immutable selection owners remain dependencies. Candidate sequence IDs stay unique across the
stored session. Selecting a count whose cache exists automatically uses its saved set. A reference conflict is
shown before queue mutation.

Inspection always reads the highlighted row's own bundle/manifest. Ordinary rows show their captured source
times and thumbnails, without invented timeline, decoded sequence or player scores. Partial ordinary captures
may be inspected as partial after failure; a completed row requires its complete manifest. Players rows show
the existing exact identities, scoring provenance and fingerprint plus images from the immutable bundle.

## Promotion and publication

The library copies and validates the complete selected input bundle into the destination game before publishing
the config/maps generation that references its fingerprint. It holds the relevant source/destination artifact
and config locks using the existing lock order. Because bundles are immutable and independently identified,
an interrupted promotion may leave an unreferenced complete bundle, but cannot publish a config that points to
partial images. Existing transactional publication of maps, panorama, generation and configuration remains the
commit point. A copied bundle alone does not mean the candidate was selected.

For an ordinary candidate, the UI/backend promotion wrapper requests copying the bounded inspection manifest
and JPEGs from inside the existing locked promotion callback/library phase, before the same config commit.
The directory and manifest bind the calibration invalidation ID actually present in the selected config (currently
the candidate ID); Hugin's fresh artifact generation is a separate identity. Copying outside the transaction
would race the selected owner and is insufficient. Validate the candidate owner and complete pair count and do
not reinterpret `left.png`/`right.png` as the full set. Legacy candidates without inspection data may still promote
their validated maps; reopening reports inspection unavailable.

Do not acquire the persistent-store lock while holding nested promotion artifact/config locks. Finish bundle
installation and transactional main publication first, then update the catalog separately. The main config and
generation are authoritative if the later catalog update fails; report that bookkeeping failure without claiming
the main publication rolled back.

The close guard offers Use selected / Keep results and close / Cancel for unapplied completed candidates or
frozen plans, with Cancel the default. Use requires a ready, stopped, nonquarantined candidate and closes only after
successful transactional promotion. Cache retention is independent of whether the main configuration changed.

## Validation required before implementation is considered complete

- Publish/load/copy all selected full images with exact fingerprint/content checks; reject partial, corrupt,
  oversized, unsafe-path and wrong-owner bundles without fallback.
- Reopen a promoted Players calibration and inspect every pair without a baseline, scan or source extraction.
- Add a same-count control-point variant in a new dialog and prove no new scan or capture occurred.
- Create and retain two counts, close/reopen, switch back to the older count and reuse its exact fingerprint.
- Preserve a frozen set after solve failure, reject quarantined ownership, and handle interrupted index writes.
- Promote an ordinary baseline, reopen it, and inspect that baseline's actual complete captured set.
- Exercise game-local ownership regardless of output-root overrides, distinct same-named games, explicit and
  interrupted cache removal, main-plan precedence and numeric reference conflicts. Preserve existing x86/Jetson builds, real GPU promotion-to-Program reuse, and review cycles.

The real GPU persistence workflow passes on a disposable game on NFS: baseline generation, one search with
20 observations and two selected pairs, two solves sharing those pairs, per-row inspection, paced previews,
promotion and automatic close, and five seconds of Main Program without recalibration. Reopening inspects Main
without a runner and completes another same-count control-point variant from retained PNGs. The test checks
every full-resolution input digest, rejects any new scan or capture, and verifies unchanged Main provenance.
