# Game storage

A game directory contains its recordings, intentional published outputs, and
one application-owned `_hockeymon/` directory. The storage contract is
`hockeymon-game-state-v1`. The native workflow capability is
`game-state-directory-v1`; paired web workers require it.

There is no flat-layout fallback or migration. A root `config.yaml`, old TIFFs,
or old experiment directories do not seed a new run. Configure the game again.
The application does not delete those historical files.

```text
game/
  original recordings or cam1/, cam2/, ...
  published MP4s, telemetry databases, portable archive sidecars
  _hockeymon/
    manifest.json
    .manifest.lock
    config.yaml
    rink-map.yaml
    highlights.json
    highlight-assets/
    hm_project.pto, autooptimiser_out.pto
    mapping_*.tif, panorama.tif, seam_file.png, s.png, rink_mask_*.png
    stitching_generation_id, stitching_canvas_provenance
    calibration-matches/, player-frame-inputs/, rink-mask-input/
    calibration-frame-inspection/, player-frame-inspection/
    stitching-experiments/
    hstream-ui/, hstream-runtime/
    hstream-job-<run-id>.yaml
    transaction journals, locks, temporary work and publication staging
```

Existing bundle names stay intact within the private namespace. Manual matches,
selected calibration frames, experiments, configs and authoring assets are
durable work, not disposable cache. Reorganizing them does not reduce disk usage
or introduce automatic eviction.

## Path ownership

`common/GameStorage.h` exposes pure `StateDirectory(game)` and
`StatePath(game, relative)` helpers. Their game argument is always the logical
recording root. `EnsureStateDirectory` creates and validates the owned child;
it never migrates old state. A user-selected game-directory alias is supported,
but `_hockeymon` itself must not be a symlink. `EnsureStateDirectory` and
`ConfineWorkDirectory` reject a game whose own last component is `_hockeymon`,
so a caller that resolved the private directory and then passed it on cannot
create a nested copy whose locks and journals no other process reads. Only the
last component is checked: an experiment's candidate game legitimately lives
under one. A game whose own directory is named `_hockeymon` is therefore not
usable, which is the intended trade. The pure `StateDirectory`/`StatePath`
helpers are unchanged and will still build a nested path for a reader.

Normal stitching, GameConfig and HuginProject game APIs take the logical game
root. Low-level artifact-copy primitives use explicitly flat artifact
directories: `stitch_artifact_content_identity_locked`,
`validate_stitch_generation_artifact_bounds_at_directory`,
`HuginProject::ReadCanvasProvenanceAtDirectory` and temporary image-localization
helpers. Do not prefix those already-private scratch directories again.

Web request/control files live outside their private staged games and retain
their existing per-run locations. Source aliases at a staged game's root remain
available for native Auto discovery. They are explicit `input-reference` entries
and never confer ownership of their targets. Shared models and the external
output working root remain shared/external; they are not copied into every game.
If an explicitly configured working directory resolves inside the game,
`ConfineWorkDirectory` places it under `_hockeymon/work/` instead. This also
applies when an output-root plus game ID resolves back to the game itself.

## Inventory

`manifest.json` is UTF-8 JSON with `schema_version: 1`,
`layout: "hockeymon-game-state-v1"`, `updated_by`, and a `files` array. Each entry
has a game-relative `path`, `kind` (`file`, `directory`, `symlink`), `role`,
`producer`, and `state: "present"`. Regular files also have `size_bytes`;
explicit publication entries may have `run_id`.

Roles distinguish `saved-state`, `cache`, `temporary`, `recovery`, `published`
and `input-reference`. Unknown internal files are conservatively classified as
saved state. The producer is the operation that first registered/observed the
entry; it is descriptive provenance, not a verified publication receipt.
Existing bundle manifests and publication receipts retain their exact hashes,
dependencies and transaction identities; the index does not rehash large files.

`RefreshManifest` inventories private descendants without following symlinks
and preserves explicitly registered root outputs and input references.
`RegisterPublished` registers an exact output path after publication.
`RegisterInput` records an explicitly imported recording/directory/alias without
claiming ownership of its target. Arbitrary
root files and original recordings are never inferred to be owned from their
names. Removed entries disappear on refresh. Directory entries and child files
are both recorded; apparent file sizes are not deduplicated disk-usage totals.

Writers serialize read/update/replace through `_hockeymon/.manifest.lock`,
publish an exclusive temporary file with atomic rename, and fsync the result.
The manifest itself is excluded to avoid self-referential size updates.
Abandoned `.manifest.tmp-*` files are inventoried as temporary state; the current
atomic update is created only after the scan. Inventory limits are 100,000 entries,
64 directory levels and a 32 MiB document. Refresh runs at configuration,
calibration, authoring, staging, publication and cleanup boundaries, never from
video-frame processing or browser polling. Active operations may have newer
files until their next boundary.

The index cannot authorize cleanup, establish calibration validity, or complete
a run. Existing journals, immutable bundle receipts and publication records
remain authoritative. The mutable inventory and its lock must not enter an
immutable calibration receipt. Same-filesystem publication staging stays under
the game's `_hockeymon` so atomic publication and recovery keep their filesystem
requirements.

Native archive runs require the current managed publication protocol. A runner
that exits without the completed run manifest is rejected and its work is
retained; it cannot fall back to the older game-root finalizer layout.
