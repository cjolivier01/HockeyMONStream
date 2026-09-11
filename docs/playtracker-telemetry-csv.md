# Play-tracker telemetry CSV export

HStream can save the detector/tracker inputs and camera-policy outputs used by
HockeyMOM camera-model and DriveGPT tooling. In `hstream-ui`, select **Save
DriveGPT CSVs** before starting a Program run. The UI stages the run under its
output working root, normally `~/hstream_output/<game-id>` (or
`$HM_OUTPUT_WORK_DIR/<game-id>`). Direct `pipeline-app` runs can opt in with:

```text
--options=pipeline.ds-playtracker.private-properties.telemetry-csv-dir=/path/to/working-directory
```

The directory must not contain `=` or `;`, because DeepStream private plugin
properties use those characters as delimiters. Export is disabled by default
and is not enabled for stitching-calibration-only runs.

## HockeyMOM compatibility

Each run reserves one generation across all artifacts. Bare filenames are used
only when none of the corresponding artifacts already exists; otherwise the
new generation is greater than every existing numeric suffix. Generation
selection is serialized by a directory lock, and every artifact is created
with exclusive, no-symlink-following semantics. Existing files are never
truncated or replaced.

The four HM files are headerless, as expected by `../hm`:

| File | Columns | Meaning |
| --- | --- | --- |
| `detections[-N].csv` | `Frame,BBox_X1,BBox_Y1,BBox_X2,BBox_Y2,Scores,Labels` | Primary detector output in TLBR coordinates, captured immediately after primary inference and before rink filtering and tracking. HStream applies no additional export score threshold; rows already reflect the primary inference/parser configuration's confidence filtering. |
| `tracking[-N].csv` | `Frame,ID,BBox_X,BBox_Y,BBox_W,BBox_H,Scores,Labels,Visibility,JerseyInfo,ActionLabel,ActionScore,ActionIndex` | Current 13-column `TrackingDataFrame` schema. HStream supplies tracked player ID, score, class, and TLWH; unavailable jersey/action annotations use HM's neutral values. |
| `camera[-N].csv` | `Frame,BBox_X,BBox_Y,BBox_W,BBox_H` | Follower/Program camera action in TLWH coordinates. |
| `camera_fast[-N].csv` | `Frame,BBox_X,BBox_Y,BBox_W,BBox_H` | Fast camera-policy action in TLWH coordinates. For a one-box policy it matches `camera.csv`. |

`Frame` is a monotonic, one-based sample ID shared by all four files. Queue
saturation does not consume or lose an ID: it blocks the producer until the
writer has space. A seek and every live policy change deliberately reserve an
unused ID, so policy/timeline boundaries remain visible as numeric gaps. A
frame with no detections or tracks naturally has no row in that sparse file;
the frame-index sidecar records its zero count. These gaps prevent training
windows from silently joining unrelated timesteps.

If a pipeline intentionally has no primary inference element, HStream records
an explicit empty detection snapshot for every frame. If primary inference is
configured but its post-inference snapshot metadata is missing, the run fails
instead of silently writing incomplete detection data.

## Reproduction and provenance sidecars

The working generation also contains:

- `hstream_frame_index[-N].csv`: sample ID, source ID, native DeepStream frame
  number, decoded source/sequence when present, buffer PTS, NTP timestamp, seek
  epoch, canvas dimensions, detection count, track count, and camera-action
  availability.
- `hstream_replay[-N].jsonl`: exact ordered native TLBR policy inputs, arena,
  source/reset identity, whether the policy stepped, original crop rotations,
  and periodic/boundary checkpoints immediately before a frame. Checkpoints
  preserve resolved native configuration, pan/zoom/braking state, and player
  history; the existing HM CSV formats are unchanged.
- `hstream_config_events[-N].csv`: the sample boundary for seek events, live
  camera geometry changes, base-config reloads, and runtime tuning updates.
- `play_tracker_source[-N].yaml` and `play_tracker_effective[-N].yaml`: exact
  startup policy configuration. Runtime tuning YAMLs are copied alongside the
  event that activated them.
- `hstream_telemetry[-N].json`: schema declaration, filenames, original config
  paths, run outcome, publication state, persistence counts, and backpressure
  counters.

`writer_drained` reports that the writer queue was finalized. `completed`
requires an explicit successful EOS or intentional-stop outcome, a healthy
writer, durable files, and committed non-hidden CSV names. A completed
generation can still have `eligible_for_training: false` when it contains no
sample with both tracks and a camera action; the CSVs remain available so an
empty sparse file is not mistaken for a missing export. Fatal pipeline results
and incomplete teardown remain pending/incomplete and do not publish the HM
files.

`samples_buffered`, `training_samples_buffered`, and
`config_events_buffered` describe data accepted by the writer. Their
`*_persisted` counterparts are updated only after final flush and `fsync`, so
an ENOSPC or other final I/O failure cannot claim removed rows were durable.
`queue_full_waits` counts times producers encountered a full queue;
`dropped_samples` and `dropped_config_events` remain zero for the lossless
queue. Config-event counters separately report applied and persisted policy
boundaries.

## Working and game-directory publication

While a run is active, its four HM inputs are written to hidden `.partial`
files in the working directory, so filename-based consumers cannot select an
incomplete generation. Sidecars, provenance, and the manifest also remain in
working storage. After the writer drains successfully, all files are flushed
and synchronized. HStream creates no-replace hard links for `camera`,
`camera_fast`, and `detections`, synchronizes the directory, then links
`tracking` last as HM's discovery marker. It removes the hidden working names
after the non-hidden links are committed.

When `hstream-ui` finishes the first video archive, it copies these six non-hidden
CSV files into the game directory:

- `detections[-N].csv`
- `tracking[-N].csv`
- `camera[-N].csv`
- `camera_fast[-N].csv`
- `hstream_frame_index[-N].csv`
- `hstream_config_events[-N].csv`

New recordings also publish `rink_mask_0-N.png`, captured from the immutable
CPU mask actually used by the filter, together with the replay JSONL, telemetry manifest, startup
configuration YAMLs, and all configuration artifacts referenced by events.
The copied manifest and event artifact references are rewritten to the game
generation; original working files keep their original bytes and names.
Every replay/config companion is synchronized before the final `tracking`
commit marker. Legacy generations without a replay sidecar retain the
six-file publication behavior.

The game files are independent copies, not links back to working storage. Their
suffix is taken from the finalized
`<game-id>-tracking_output-with-audio-N.mp4` (or the stitched archive when
only that archive is saved), even when the working telemetry generation used
a different suffix. New game-directory archives start at `-1`. Their number is
one greater than the highest existing tracking video, stitched video, CSV,
manifest, or mask snapshot number; gaps are never reused. For example, videos
numbered `-1`, `-3`, and `-4` advance to `-5`. Program and stitched archives from
one run share the same suffix. Existing bare filenames remain unchanged. After the video commit and identity-guard cleanup
finish, a background finalization worker copies and synchronizes each CSV to an
unnamed inode in the game filesystem. It then atomically links the final
companion names, synchronizes the directory, and atomically links `tracking`
last. A final `tracking` name is therefore never visible while its bytes are
still being copied. No hidden telemetry files are staged in the game directory,
and an existing destination is never overwritten. If final copying fails, the
complete non-hidden working generation is retained and the UI logs its path.

Runs without a finalized video archive remain available only in working
storage because there is no video suffix to assign. Failed or interrupted runs
retain the manifest and audit sidecars but remove their owned hidden HM staging
files. A process crash can leave `.partial` files in its configured working
directory for forensic recovery; they are never treated as a committed
generation.

## GPU-path and backpressure contract

The exporter copies only CPU-resident `NvDsObjectMeta`, frame timestamps, and
the native playtracker's small result structures. It never maps or reads an
`NvBufSurface`, performs a device-to-host pixel copy, or introduces a CPU image
conversion.

The run mask is encoded once from the filter's already-loaded CPU calibration
image and queued with the first telemetry sample. It does not trigger a D2H
transfer or snapshot a video frame. A mask change within one recording fails
telemetry publication rather than attaching an incorrect single mask.

Steady-state CSV file I/O runs on a dedicated writer thread behind a bounded
queue (2,048 complete frame samples by default). A sample is queued for every
processed frame; export is not reduced to every Nth step. If the queue becomes
full, the streaming/config producer blocks until space is available and emits
a one-time warning to the log. It never drops a frame sample or config event.
If an accepted config event cannot be written with its required provenance,
the generation is failed and its HM training inputs are not published.

## Camera experiments

See [Camera experiments](camera-experiments.md) for selecting a short range,
restoring its historical state, and comparing camera parameter trials. The
replay reader accepts only a completed recording. A camera CSV alone contains
positions and sizes, not the motion and player history required for restoration.

Checkpoints and exact inputs are metadata only. Capturing them does not map or
read video pixels. They share the existing bounded, lossless writer queue and
are flushed and synchronized before the completed generation is committed.
A native checkpoint is tied to its explicit schema/implementation version;
incompatible snapshots are rejected. Older CSVs require explicit archived
arena geometry and measured reconstruction agreement instead of silently
initializing the camera from defaults. The Experiment media binding separately
identifies an uncropped panorama and the relationship between its PTS and the
recording's PTS; matching image dimensions alone do not prove source identity.
