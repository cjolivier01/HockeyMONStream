# Lossless NVIDIA stream batching

`hstreamlosslessmux` is built from the installed DeepStream replacement mux,
registered privately by `apps-common/HStreamLosslessMux.cpp`. It batches one or
more inputs without copying their video pixels to the CPU. `create_multi_source_bin`
selects it for recorded URI playlists, including single-source stitched-output
playback. Ordinary live-camera sources and `nvmultiurisrcbin` keep their existing
SDK paths.

“Lossless” means a wall-clock batch deadline never forces an incomplete batch.
With adaptive batching, each still-active input contributes to the batch. A
source stops contributing only after its queued buffers and EOS have drained.
Empty inputs and unequal input lengths therefore terminate without dropping
another input's tail. Cancellation wakes the source task before pad deactivation.
Timestamp synchronization retains the SDK's configured late-frame policy; use
`sync-inputs=0` for exact recorded frame retention.

The decoder rendezvous and shortest-recording termination used by stereo
stitching are separate from mux selection. They still apply to exactly two
recorded cameras. Other input counts drain each playlist independently.
Hstream's calibration and `hmstitcher` adapter still accept two cameras only;
`hm-cupano/tests/stitch_three_videos.cpp` uses a separate three-camera stitcher.
N-input mux support does not expand that calibration or stitching contract.

## SDK compatibility

The element retains the replacement mux's properties, request pads, GPU surface
references, per-frame metadata, timestamps, source IDs, and NVIDIA stream events.
Its patches add P010 caps, prevent deadline-driven partial batches, keep event
processing live while waiting for peers, and fix initialization/shutdown locking.
Clock-based synchronization and rate control can retry without a new buffer;
those retries do not authorize incomplete output.

This is not the legacy scaling mux: width/height, padding, memory conversion,
and the legacy buffer pool properties are not applied to it.
`set_streammux_properties` checks the actual element factory before applying
legacy-only properties. Converters remain responsible for format/size changes.
No AMD path uses this NVIDIA SDK element; AMD decoding and stitching are unchanged.

## Regression coverage

- `//src/apps/apps-common:hstream_lossless_nvstreammux_inputs_test`: real GPU
  inputs with one through four sources, empty/unequal inputs, delayed frames and
  EOS, cancellation, repeated playback, sparse pad IDs, synchronization, NV12,
  RGBA and P010. Checks complete membership, frame counts/order, PTS and metadata.
- `//src/apps/apps-common:uri_multiple_mux_inputs_test`:
  production URI playlist decoding and audio across chapters with one through four
  inputs, including unequal three-input playlists.
  The existing stereo cases exercise the exact decoder rendezvous and audio.
- `hstream_lossless_nvstreammux_caps_test` and
  `hstream_lossless_nvstreammux_rejected_caps_test`: P010 negotiation and
  shutdown after rejected caps.
- `//src/gst-plugins/gst-videoprep/stitcher:one_pass_stitcher_test`: stereo
  pairing, continuity, partial/EOS handling and output sizing.
