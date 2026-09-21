# Stitching Experiments

The desktop UI's **Stitching experiments…** tool compares multiple stitching calibrations against the same game and
the same moving playback passage. It runs only while the main pipeline is stopped.

## Candidate matrix

Enter comma-separated values for:

- control-point limits (20–5000);
- calibration frame counts (1–16); and
- first calibration-frame timestamps (`HH:MM:SS` or `HH:MM:SS.mmm`).

The tool generates the Cartesian product, with a maximum of 64 candidates. By default every candidate reuses the
game's saved rink-leveling rotation. Clear that option to add pitch/roll variants as comma-separated `pitch/roll`
pairs, for example `0/0,-1.5/0.5`. Explicit variants preserve the game's saved yaw.

Each candidate receives a private temporary game directory. Camera chapters and matching camera-calibration sidecars
are read-only symlinks to the selected game, while generated stitching artifacts and configuration stay isolated.
Candidates run serially through `hstream-cli --stitching-calibration-only` with a fake sink. This graph omits Program
crop, inference, rink masking, and play tracking.

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

Selection does not rerun feature matching, optimization, map generation, or seam generation. The next Program run
therefore loads the chosen artifact generation directly. Closing the dialog removes unselected temporary candidates;
camera videos are never copied or modified.
