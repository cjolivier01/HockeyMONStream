# Stitching Experiments

The desktop UI's **Stitching experiments…** tool compares multiple stitching calibrations against the same game and
the same moving playback passage. It runs only while the main pipeline is stopped.

## Candidate matrix

Enter comma-separated values for:

- control-point limits (20–5000);
- calibration frame counts (1–16); and
- first calibration-frame timestamps (`HH:MM:SS` or `HH:MM:SS.mmm`).

**Add options to batch** adds the Cartesian product to a visible queue without starting calibration. Change the
fields and add more combinations, or remove individual queued rows, until the batch is ready. Identical additions
in one dialog are ignored while the main configuration is unchanged; reopening allows another solve. A batch can
contain at most 64 queued candidates. By default every candidate reuses the game's saved
rink-leveling rotation. Clear that option to add pitch/roll variants as comma-separated `pitch/roll` pairs, for
example `0/0,-1.5/0.5`. Explicit variants preserve the game's saved yaw.

Adding options prepares each private game directory on a worker and saves the queued configuration in the
persistent experiment cache. Camera chapters and matching
camera-calibration sidecars are input symlinks to the selected game, while generated stitching artifacts and
configuration stay isolated. The dialog remains responsive during preparation; queued rows survive closing it.
**Start batch** locks the queue and runs its candidates serially through
`hstream-cli --stitching-calibration-only` with a fake sink, so the batch can be left unattended. This graph omits
Program crop, inference, rink masking, and play tracking. Completed candidates remain available for comparison after
the batch finishes or is cancelled.

**Prefer player-rich frames** is enabled by default and adds an automatic candidate alongside its ordinary baseline
for multi-frame calibrations. Uncheck it to queue only ordinary candidates when no saved frame selection applies. The search starts
at the first calibration frame and defaults to 60 seconds, bounded to 300 seconds. It samples every 500 ms through
the existing detector and Program ice-mask pruning; people surviving that filter include players and referees.
Tracking is not required. The baseline prepares its rink mask before scanning, while the scan requires that exact
existing mask and adds no video readbacks. Selected synchronized pairs are replayed exactly for a separate calibration.
The first pair remains the anchor, and a one-frame candidate needs no scan. All variants with the same frame count share one frozen selection: changing control-point limits or rotation
runs only another solve. The first search duration is authoritative for that count. A different frame count can
establish another selection; an incompatible reference time for an established count is an error. Baselines count toward the 64-row limit; removing one removes its dependent automatic rows.

An empty passage or insufficient separated people leaves the automatic row unavailable with a reason. Source,
model, inference, or mask failures are errors. The ordinary baseline remains available. Automatic frame selection
does not guarantee improved alignment or that the feature matcher uses player points; compare the moving results.

Adding, running, cancelling, previewing, or discarding a batch never modifies the selected game's stitching config or
artifacts. Closing retains the complete experiment history, configurations, extracted frames and logs under the game’s
`stitching-experiments/` directory. Reopening restores it. **Discard experiments** explicitly removes this owned
history after confirming its runners have stopped; the directory can also be removed manually while all experiment
dialogs and runners are closed. There is no automatic eviction. Promoted inputs live separately in the main game and survive
experiment-cache removal. The only operation that changes
the main game's stitching state is the explicit **Use selected in main Program** action described below. If a stopped
runner's isolated process session cannot be confirmed dead, the tool reports and retains its temporary workspace path
instead of risking deletion while a descendant still uses it; that retained directory can be removed after the
process exits. A retained candidate is quarantined from preview and promotion.

## Comparing candidates

Select a ready row and choose **Play selected** (or double-click it). The existing GPU-native stitched preview replays
the configured passage start and duration; Loop restarts that exact passage. The candidate matrix and results sit
beside the preview, with passage start and duration on separate labeled rows below it.

Drag a window edge or the bottom-right resize grip to resize the dialog, or use its title-bar maximize button.
The calibration-frame inspector has its own resize grip and resizes independently of the experiment window;
camera and match images scale to fit. **Expand preview** (or double-click the video) hides
the candidate panel and log to give the moving canvas more space while keeping playback controls available.
**Restore layout**, another double-click, or **Escape** returns to the previous split without restarting playback
or replacing its native GPU window. Switching candidates starts the same passage against that candidate's maps and
seam. Video surfaces remain GPU-resident.
Calibration-only embedded playback retains the render sink's configured clock pacing, so a passage plays at normal
speed. Ordinary Program previews keep their existing processing/encoding timing.

**Inspect selected frames** opens the highlighted row’s actual inputs. Ordinary baselines show their captured
camera images and source times, including partial captures when solving fails; they do not borrow a Players row’s
images or display invented people scores. A Players row’s ordered frame list includes the
anchor, exact physical source files and nanosecond timestamps, decoded sequences, eligible-person counts, apparent
far/middle/near counts, and selection quality. It also checks whether the source files still match their recorded
identities. The colored 16×9 grid shows scoring coverage in the **baseline stitched canvas**; it does not overlay
stitched detections onto raw-camera images or imply metric depth.

Reopening the dialog offers the current main calibration and retained experiment rows for inspection. A new
same-count Players variant uses the saved PNGs without another search or selected-frame extraction. Other cached
counts remain available when requested again. Source/reference conflicts and corrupt or deleted required inputs
fail explicitly. Older plans created before input persistence may replay their exact selected timestamps once to
materialize the missing PNG bundle; opening the inspector never does that work.
**View runner log** shows the highlighted row's retained output, including after reopening. The viewer reads at
most the last 1 MiB; the complete log stays with that candidate on disk.

Each pair has left/right thumbnails from the exact images extracted for matching. They become available when that
pair is extracted and remain inspectable if solving subsequently fails. Thumbnail creation reuses the CPU images
already required by calibration, without another video-surface readback or a separate approximate seek. Each is at
most 1024 pixels on its longest edge. New selected sets retain all full-resolution PNGs and inspection JPEGs under
`player-frame-inputs/<selection fingerprint>/`; the complete set is copied into the main game during promotion.
Ordinary rows retain a bounded source manifest and JPEGs under `calibration-frame-inspection/<invalidation id>/`. Missing thumbnails are shown explicitly rather than replaced with nearby frames.

In **Inspect selected frames**, enable **Show matches** to see the two camera stills with green lines connecting
matched control points. **Points only** hides the lines while retaining the matched endpoints. These are the
matcher-selected correspondences for that individual pair, after its confidence/filtering and control-point cap,
before multi-frame pooling and geometric validation; they are not all raw SuperPoint detections or the final
solver's inliers. Changing the selected pair updates the view, and turning off Show matches restores the original
camera thumbnails. The coverage grid keeps its separate meaning.

The control-point maximum limits retained matches rather than the detector's raw
keypoint count. Capped selection balances occupied height bands before horizontal
cells to preserve available near-side points when the back wall is more textured.
It cannot add lower-image matches that the detector did not find, and a maximum
above the accepted count retains all of them. See [native feature matchers](native-feature-matchers.md)
for the detector limits, selection algorithm, and comparison with HockeyMON.

Calibration saves `points_N.jpg` and `matches_N.jpg` beside the generation's ordinary inspection files, including
for Players rows. Each combined image is at most 2048 × 1024 pixels and uses the CPU stills and matching results
already available during calibration. Calibrated AKAZE coordinates are projected back through the lens model so
the overlays align with the raw cameras. No extra inference, video decode or video-surface readback is needed.
Unlike the reusable selected input bundle, these pictures belong to a specific solve. Retries clear previous
match pictures before matching, and promotion copies them to the main game. Existing experiments and pairs that
failed matching may lack these pictures; the inspector reports that explicitly and never regenerates them.

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
not prompt for an unrelated crop review against the previous calibration. Failed publication leaves the dialog open with the error.
Closing with unapplied results, including through the window close button or Escape, offers **Use selected**,
**Keep results and close**, and **Cancel**. Cancel is the default; keeping results preserves them for the next visit.
The separate **Discard experiments and cache…** action explicitly deletes the retained history after confirmation.
Failed promotion keeps the results available. Publishing large generations
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
It also checks one scan across control-point variants, same-count selection inheritance, per-row baseline/Players
inspection, dependency removal, the full queue bound, bootstrap failure and cancellation, and the Close confirmation. Backend tests distinguish invalid reports from unavailable coverage and preserve selection
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
It then reopens the dialog, inspects Main without a runner, and solves another same-count option from retained PNGs,
checking every input digest and requiring no new player search or frame capture.
`HSTREAM_TEST_PLAYER_ANCHOR=HH:MM:SS[.mmm]` selects the action passage for that mode. The input must contain enough
on-ice people for the requested pair count; unavailable coverage is intentionally a failed smoke test.

Re-leveling remains available for completed calibration, including the current version-10 metadata. Saving revised
angles retains the selected frame plan, input bundle, and optimized NONA alignment. Rebuilding the view runs only
projection, maps, seam, panorama, and dependent rink work. Cancel/retry keeps that source alignment; it cannot fall
back to matching. Legacy projects require AUTO canvas for this operation. Changing a solve input, such as the
control-point budget, requests a new solve using the retained frames. Selecting an ordinary historical candidate
at the same frame count cannot silently remove Main's player selection.

See [the alignment preservation design](preserve-stitching-alignment-design.md) for ownership and retry behavior.
