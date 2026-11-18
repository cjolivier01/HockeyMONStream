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

- Make `hstreamlosslessmux` work with a single input. Today it only handles two
  cameras: `create_multi_source_bin` in `deepstream_source_bin.cpp` selects it
  on `uri_playlist_source_count == 2` and anything else gets the stock mux.
  Point it at one source and the pipeline stalls — `uri_multiple_pipeline_test`
  goes from 20s to a 300s timeout, hanging in `run_single_uri_multiple_source`,
  the stitched-output playback case. Likely in the first hunk of
  `nvstreammux_lossless.patch`, which drops the forced partial-batch flush so a
  batch only leaves on `is_ready()`; with one source that never becomes true.
  Worth fixing because single-source playback is then stuck on the stock mux,
  and so misses the `P010_10LE` caps the same patch set adds for 10-bit. Two
  assertions pin the current split and would need updating:
  `UriMultiplePipelineTest.cpp` and `PluginPropertyYamlParserTest.cpp`.

## Misc

- Deferred: add an optional arbitrary output label after the game ID in output
  filenames, similar to HockeyMON's `--label` / `--output-label` support (for
  example, `<game-id>-<label>-program_8k_output-with-audio-1.mp4`). Revisit how to
  expose it without cluttering the already busy UI; no implementation for now.

