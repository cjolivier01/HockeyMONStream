# Stitching Experiments

The desktop UI's **Stitching experiments…** tool compares multiple stitching calibrations against the same game and
the same moving playback passage. It runs only while the main pipeline is stopped.

## Candidate matrix

Enter comma-separated values for:

- control-point limits (20–5000);
- calibration frame counts (1–16); and
- first calibration-frame timestamps (`HH:MM:SS` or `HH:MM:SS.mmm`).

**Add options to batch** adds the Cartesian product to a visible queue without starting calibration. Change the
fields and add more combinations, or remove individual queued rows, until the batch is ready. Duplicate combinations
are ignored and a batch can contain at most 64 candidates. By default every candidate reuses the game's saved
rink-leveling rotation. Clear that option to add pitch/roll variants as comma-separated `pitch/roll` pairs, for
example `0/0,-1.5/0.5`. Explicit variants preserve the game's saved yaw.

When **Start batch** reaches a candidate, it prepares a private temporary game directory. Camera chapters and matching
camera-calibration sidecars are input symlinks to the selected game, while generated stitching artifacts and
configuration stay isolated. Workspace preparation is deferred so adding a large option matrix remains immediate.
**Start batch** locks the queue and runs its candidates serially through
`hstream-cli --stitching-calibration-only` with a fake sink, so the batch can be left unattended. This graph omits
Program crop, inference, rink masking, and play tracking. Completed candidates remain available for comparison after
the batch finishes or is cancelled.

Adding, running, cancelling, previewing, or discarding a batch never modifies the selected game's stitching config or
artifacts. **Discard batch** and closing the dialog remove the private candidate data. The only operation that changes
the main game's stitching state is the explicit **Use selected in main Program** action described below. If a stopped
runner's isolated process session cannot be confirmed dead, the tool reports and retains its temporary workspace path
instead of risking deletion while a descendant still uses it; that retained directory can be removed after the
process exits. A retained candidate is quarantined from preview and promotion.

## Comparing candidates

Select a ready row and choose **Play selected** (or double-click it). The existing GPU-native stitched preview replays
the configured passage start and duration; Loop restarts that exact passage. Use **Maximize for seam inspection** to
give the moving stitched canvas more screen space. Switching candidates starts the same passage against that
candidate's maps and seam. Video surfaces remain GPU-resident.

## Selecting the Program calibration

**Use selected in main Program** validates and transactionally republishes the candidate's existing Hugin project,
mapping TIFFs, seam, panorama, source stills, and canvas provenance under a fresh destination generation identity. It
then merges the candidate's stitching-owned settings into the selected game's `config.yaml` and invalidates dependent
rink-mask/output state. Artifact and config publication share their locks and a durable recovery boundary: an
interruption before the selected config is durable restores the prior artifact generation, while an interruption after
it is durable retains the matching promoted generation.

Validation and durable publication run on a worker so the desktop remains responsive; conflicting experiment actions
and dialog closure wait for that transaction to finish.

Selection does not rerun feature matching, optimization, map generation, or seam generation. The next Program run
therefore loads the chosen artifact generation directly. Discarding the private batch afterward does not affect that
promoted copy; camera videos are never copied or modified.
