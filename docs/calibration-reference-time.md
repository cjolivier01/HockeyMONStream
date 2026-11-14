# First calibration frame

`stitching.stitch_frame_time` accepts `[-]HH:MM:SS[.mmm]` in the desktop,
CLI, browser and Stitching Experiments. For example, `-00:00:01` selects the
frame one second before the common synchronized recording ends. The common
recording excludes each camera's effective source start and ends when either
camera runs out of video. A playback duration limit does not change that end.
Negative zero is invalid because no frame begins at the recording end.

Zero inherits the main UI's Playback start. The main editor displays that start
and follows later edits until a nonzero reference is entered; presets retain zero
as the inheritance marker. Stitching Experiments initializes its matrix from the
effective reference and freezes both the reference and Playback start when queued.
Explicit positive and negative references remain unchanged. Entering zero in a
new matrix also resolves to its frozen Playback start.

For an end-relative reference, the first pair stays at that reference. With
N pairs, the other positions advance by one Nth of the usable interval from
Playback start to the common end and wrap to Playback start, not zero. A reference
before Playback start samples only its remaining tail, without revisiting frames.
Chapter boundaries and camera starts are resolved independently. Seeking selects the first decodable frame at the requested
time; a position inside the final frame interval uses that last frame.
Positive references retain their existing forward sampling behavior.

Prefer player-rich frames is available for either reference sign. An end-relative
scan runs to EOF, then resumes at the Playback start frozen when the candidate
was queued (00:00:00 by default), using the remaining search duration. It wraps
at most once and stops before revisiting the original anchor. Selected pairs
retain their physical source paths and decoded timestamps for exact replay;
changing Playback start later does not change an existing selection. Without
player selection, end-relative candidates retain circular sampling.

`PipelineApp` resolves the live initial seek so the calibration preview shows
the reference pair and supplies the actual launch start to `hmstitcher` through
`calibration-playback-start-ns`. Both local and distributed experiment calibration
commands pass their frozen start. `CalibrationReferenceFrames` probes chapter headers and
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

Level Rink accepts version 16/17 per-camera calibration metadata and checks both
source-bound optics before opening the saved images. It no longer rejects a
completed independent-camera calibration as an unsupported format.
Saving a leveling or crop change compares optics with the canvas metadata;
source start times remain bound separately by the completed backend generation
and the reframe solver snapshot. An unchanged start override therefore permits
reuse of the alignment, while changed starts still require a new solve.
