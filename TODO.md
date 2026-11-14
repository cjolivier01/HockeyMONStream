### Tasks

## GUI

- Deferred: resume rink panorama flattening from the pushed `feat/rink-warp-editor`
  branch. The interactive GPU still editor is implemented; applying the saved warp
  to playback and final video remains unfinished. Resume with game/calibration-bound
  settings and consistent rink, tracking, scoreboard and camera-framing geometry.
  Native editor: https://github.com/cjolivier01/HockeyMONStream/pull/350.
  Companion changes use the same branch name:
  https://github.com/cjolivier01/HockeyMONWeb/pull/89 and
  https://github.com/cjolivier01/HockeyMON/pull/240.

## CLI

## Misc

- Deferred: add an optional arbitrary output label after the game ID in output
  filenames, similar to HockeyMON's `--label` / `--output-label` support (for
  example, `<game-id>-<label>-program_8k_output-with-audio-1.mp4`). Revisit how to
  expose it without cluttering the already busy UI; no implementation for now.

