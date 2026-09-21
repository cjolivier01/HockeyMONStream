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

Enable **Prefer player-rich frames** to add an automatic candidate alongside its ordinary baseline. The search starts
at the first calibration frame and defaults to 60 seconds, bounded to 300 seconds. It samples every 500 ms through
the existing detector and Program ice-mask pruning; people surviving that filter include players and referees.
Tracking is not required. The baseline prepares its rink mask before scanning, while the scan requires that exact
existing mask and adds no video readbacks. Selected synchronized pairs are replayed exactly for a separate calibration.
The first pair remains the anchor, and a one-frame candidate needs no scan. Different search durations reuse the same
baseline. Baselines count toward the 64-row limit; removing one removes its dependent automatic rows.

An empty passage or insufficient separated people leaves the automatic row unavailable with a reason. Source,
model, inference, or mask failures are errors. The ordinary baseline remains available. Automatic frame selection
does not guarantee improved alignment or that the feature matcher uses player points; compare the moving results.

Adding, running, cancelling, previewing, or discarding a batch never modifies the selected game's stitching config or
artifacts. **Discard batch** and closing the dialog remove the private candidate data. The only operation that changes
the main game's stitching state is the explicit **Use selected in main Program** action described below. If a stopped
runner's isolated process session cannot be confirmed dead, the tool reports and retains its temporary workspace path
instead of risking deletion while a descendant still uses it; that retained directory can be removed after the
process exits. A retained candidate is quarantined from preview and promotion.

## Comparing candidates

Select a ready row and choose **Play selected** (or double-click it). The existing GPU-native stitched preview replays
the configured passage start and duration; Loop restarts that exact passage. The candidate matrix and results sit
beside the preview, with passage start and duration on separate labeled rows below it.

Use the title-bar maximize button to enlarge the whole dialog. **Expand preview** (or double-click the video) hides
the candidate panel and log to give the moving canvas more space while keeping playback controls available.
**Restore layout**, another double-click, or **Escape** returns to the previous split without restarting playback
or replacing its native GPU window. Switching candidates starts the same passage against that candidate's maps and
seam. Video surfaces remain GPU-resident.
Calibration-only embedded playback retains the render sink's configured clock pacing, so a passage plays at normal
speed. Ordinary Program previews keep their existing processing/encoding timing.

After an automatic scan has selected frames, **Inspect selected frames** opens its ordered frame list, including the
anchor, exact physical source files and nanosecond timestamps, decoded sequences, eligible-person counts, apparent
far/middle/near counts, and selection quality. It also checks whether the source files still match their recorded
identities. The colored 16×9 grid shows scoring coverage in the **baseline stitched canvas**; it does not overlay
stitched detections onto raw-camera images or imply metric depth.

Each pair has left/right thumbnails from the exact images extracted for matching. They become available when that
pair is extracted and remain inspectable if solving subsequently fails. Thumbnail creation reuses the CPU images
already required by calibration, without another video-surface readback or a separate approximate seek. Each is at
most 1024 pixels on its longest edge; only the selected set of at most 16 pairs is retained privately under
`player-frame-inspection/<selection fingerprint>/`. These inspection images are discarded with the experiment and
are not promoted into the game. Missing thumbnails are shown explicitly rather than replaced with nearby frames.

## Selecting the Program calibration

**Use selected in main Program** validates and transactionally republishes the candidate's existing Hugin project,
mapping TIFFs, seam, panorama, source stills, and canvas provenance under a fresh destination generation identity. It
then merges the candidate's stitching-owned settings into the selected game's `config.yaml` and invalidates dependent
rink-mask/output state. Artifact and config publication share their locks and a durable recovery boundary: an
interruption before the selected config is durable restores the prior artifact generation, while an interruption after
it is durable retains the matching promoted generation.

Validation and durable publication run on a worker so the desktop remains responsive; conflicting experiment actions
and dialog closure wait for that transaction to finish. Successful selection closes the dialog automatically and
returns to the main Program. Selection also accepts the candidate’s existing crop geometry, so the next Play does
not prompt for an unrelated crop review against the previous calibration. Failed publication leaves the dialog open with the error. Publishing large generations
to network storage can take time because it copies and validates artifacts and prepares durable rollback backups.

Selection does not rerun feature matching, optimization, map generation, or seam generation. The next Program run
therefore loads the chosen artifact generation directly. Discarding the private batch afterward does not affect that
promoted copy; camera videos are never copied or modified.
The selected pairs remain fixed across later stitching changes: control-point count, matcher, resolution, camera
lens/FOV, projection, crop and leveling all reuse the same pairs when recalibrating. A changed **Reference frame**
conflicts with the saved anchor and fails explicitly without discarding the selection. Changing **Frames** explicitly
requests a replacement frame set; the CLI follows the same rules. Missing, changed or unreplayable source frames
are errors, never permission to substitute generic frames. Selecting another candidate explicitly replaces the
calibration with that candidate’s frame policy.

## Focused validation

`//src/apps/hstream-ui:stitching_experiment_backend_test` checks workspace isolation and the selection configuration.
`//src/apps/hstream-ui:stitching_experiment_dialog_test` normally uses a failing stub runner to check queue
deduplication, serial continuation after failure, source-config isolation, and prompt closure without GPU use. It
also checks non-overlapping preview controls at 1280×820 and 1024×720, dialog maximization, and preview expand/restore
through the button, double-click, and Escape, preserving the splitter sizes and native window identity.
It also checks automatic baseline reuse, dependency removal, the full queue bound, bootstrap failure and cancellation,
and opt-in controls. Backend tests distinguish invalid reports from unavailable coverage and preserve selection
provenance through promotion.

For a real GPU check, build the dialog test and CLI with the host's CUDA architecture configuration (for example,
`--config=opt --cpu=k8 --config=blackwell` on an RTX 5090). The opt-in mode requires an X11 display and a **disposable
game copy** with local camera inputs and relative `game.videos` paths:

```bash
QT_QPA_PLATFORM=xcb bazel-bin/src/apps/hstream-ui/stitching_experiment_dialog_test \
  --gpu-smoke /path/to/disposable-game /path/to/hstream /tmp/stitch-experiment-pipeline.log
```

This runs two calibrations and two five-second previews, checks GPU presentation, clock pacing, successful runner
exit, and reuse without recalibration, verifies that the source config/artifacts stay unchanged before selection,
then explicitly promotes a candidate into the disposable game, checks automatic dialog closure, and runs five seconds
of main Program video with scoreboard overlay disabled. It requires unchanged calibration provenance and no new
frame capture or feature matching. It saves the runner log and attempts one bounded
display capture per passage. Desktop captures may be black under Xwayland even when the OpenGL framebuffer contains
video; the existing `@capture-preview-frame stitched /path/to/frame.png` runner command can verify the presented
frame directly. These one-shot diagnostic readbacks are not part of steady-state production playback.

Use `--gpu-player-smoke` instead of `--gpu-smoke` to exercise a baseline with rink-mask preparation, a ten-second
player scan, exact selected-pair replay, frame inspection with thumbnails, moving comparison and final promotion.
`HSTREAM_TEST_PLAYER_ANCHOR=HH:MM:SS[.mmm]` selects the action passage for that mode. The input must contain enough
on-ice people for the requested pair count; unavailable coverage is intentionally a failed smoke test.
