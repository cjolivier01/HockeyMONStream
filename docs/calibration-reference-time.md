# First calibration frame

`stitching.stitch_frame_time` accepts `[-]HH:MM:SS[.mmm]` in the desktop,
CLI, browser and Stitching Experiments. For example, `-00:00:01` selects the
frame one second before the common synchronized recording ends. The common
recording excludes each camera's effective source start and ends when either
camera runs out of video. A playback duration limit does not change that end.
Negative zero is invalid because no frame begins at the recording end.

For an end-relative reference, the first pair stays at that reference. With
N pairs, the other positions advance by one Nth of the common duration and
wrap to its beginning. Chapter boundaries and camera starts are resolved
independently. Seeking selects the first decodable frame at the requested
time; a position inside the final frame interval uses that last frame.
Positive references retain their existing forward sampling behavior.

End-relative candidates use this circular sampling instead of the optional
forward player-search pass. Both interfaces show that policy next to Player
search. Returning to a positive reference restores the prior checkbox choice.
Retained player selections cannot be silently relabeled as end-relative.

`PipelineApp` resolves the live initial seek so the calibration preview shows
the reference pair. `CalibrationReferenceFrames` probes chapter headers and
supplies lazy image writers to the shared calibration core. Those writers run
after generation preflight. FFmpeg input seeking starts at the preceding
keyframe and decodes forward to one PNG; it does not decode from the recording
start. Each child handles one image and shares the calibration cancellation
callback. The actual decoded integer PTS and time base are retained as source
provenance, including after wrapping.

Hardware decoding is requested with FFmpeg's automatic hardware selection.
Offline feature extraction and PNG/EXIF storage require CPU images, so this
bounded still path permits readback or software decoding when hardware decode
is unavailable. No new CPU image path enters steady playback. Stills remain
16-bit PNGs to retain high-bit-depth inputs.

The signed reference is preserved in presets, calibration identity, experiment
snapshots and saved-match context. It is not passed as a negative ordinary
playback start. AMD saved-calibration validation accepts the same identity;
fresh AMD automatic calibration retains the existing limitations in
`amd-gpu.md`.
