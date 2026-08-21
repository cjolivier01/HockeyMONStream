# Play-tracker telemetry CSV export

HStream can save the tracker inputs and camera-policy outputs needed by the
HockeyMOM camera-model and DriveGPT training tools. In `hstream-ui`, select
**Save DriveGPT CSVs** before starting a Program run. Direct `pipeline-app`
runs can opt in with:

```text
--options=pipeline.ds-playtracker.private-properties.telemetry-csv-dir=/path/to/game
```

The directory must not contain `=` or `;`, because DeepStream private plugin
properties use those characters as delimiters. Export is disabled by default
and is not enabled for stitching-calibration-only runs.

## HockeyMOM compatibility

Each run reserves one generation across all artifacts. Bare filenames are used
only when none of the corresponding artifacts already exists; otherwise the
new generation is greater than every existing numeric suffix. This matches
`hmlib.datasets.dataframe.find_latest_dataframe_file()` and never truncates a
pre-existing HM dataset. Generation selection is serialized by a directory
lock, and every core artifact is created with exclusive, no-symlink-following
semantics before any is written. Concurrent exporters receive distinct
complete generations; an independently created file or symlink makes the
candidate generation unavailable rather than becoming an overwrite target.

The three training files are headerless, as expected by `../hm`:

| File | Columns | Meaning |
| --- | --- | --- |
| `tracking[-N].csv` | `Frame,ID,BBox_X,BBox_Y,BBox_W,BBox_H,Scores,Labels,Visibility,JerseyInfo,ActionLabel,ActionScore,ActionIndex` | Current 13-column `TrackingDataFrame` schema. HStream supplies tracked player ID, score, class, and TLWH; unavailable jersey/action annotations use HM's neutral values. |
| `camera[-N].csv` | `Frame,BBox_X,BBox_Y,BBox_W,BBox_H` | Follower/Program camera action in TLWH coordinates. |
| `camera_fast[-N].csv` | `Frame,BBox_X,BBox_Y,BBox_W,BBox_H` | Fast camera-policy action in TLWH coordinates. For a one-box policy it matches `camera.csv`. |

`Frame` is a monotonic, one-based attempted-sample ID shared by all three
files. A queue drop consumes its ID, a seek reserves an unused ID, and every
live policy change reserves an unused ID before its provenance event enters
the writer queue. The policy boundary therefore remains visible even if queue
admission or later event/artifact I/O fails. A frame without player tracks is
naturally absent from `tracking.csv`. These numeric gaps preserve temporal
discontinuities instead of silently joining unrelated timesteps. HStream's
pinned HM patch makes both `CameraPanZoomGPTIterableDataset` and the legacy
`CameraPanZoomDataset` form sequences only within numerically contiguous runs
and use previous-camera state only from an adjacent prior ID; run starts use a
cold previous-camera state. HM's `camgpt_train` and DriveGPT training mode can
consume these files without a schema conversion.
As in HM's own saver, frames with no player rows are absent from
`tracking.csv`; they therefore split rather than bridge DriveGPT windows at the
tracking/camera intersection.

## Reproduction and provenance sidecars

The generation also contains:

- `hstream_frame_index[-N].csv`: export sample, source ID, native DeepStream
  frame number, decoded source/sequence when present, buffer PTS, NTP timestamp,
  seek epoch, canvas dimensions, track count, and camera-action availability.
- `hstream_config_events[-N].csv`: the sample boundary for seek events, live
  camera geometry changes, base-config reloads, and runtime tuning updates.
- `play_tracker_source[-N].yaml` and `play_tracker_effective[-N].yaml`: exact
  startup policy configuration. Runtime tuning YAMLs are copied alongside the
  event that activated them.
- `hstream_telemetry[-N].json`: schema declaration, filenames, original config
  paths, run outcome, publication eligibility, and loss counters.
  `writer_drained` only reports queue finalization; `completed` and
  `eligible_for_training` additionally require an explicit successful EOS or
  intentional-stop outcome, a healthy core writer, and at least one usable
  tracking/camera sample. EOS is accepted only from the terminal pipeline-bus
  result; a later fatal result downgrades it to `failed`. Any nonzero
  `dropped_samples` means the bounded queue could not keep up; each drop is a
  complete sample whose unused `Frame` ID splits training windows.
  `config_events_attempted`,
  `config_events_persisted`, and `config_events_lost` distinguish policy/event
  boundaries from successfully preserved provenance. Every live policy event
  increments `config_event_discontinuity_gaps` before admission, so a lost
  event cannot join training sequences across the undocumented transition. A
  config event that requires a YAML artifact is persisted only if that exact
  artifact is also written successfully.

While a run is active, its three HM training inputs are written to hidden
`.partial` files, so HM's filename-based discovery cannot select them. They are
published with no-replace filesystem links only after the writer drains, an
explicit successful outcome is recorded, the core writer is healthy, and at
least one usable policy sample exists. The finalized manifest and both camera
files precede the no-replace `tracking*.csv` link, which is the irreversible
HM discovery/commit point; no fallible eligibility step follows it. If
teardown occurs without a successful outcome, the core writer fails, or the
run is empty, the manifest and diagnostic sidecars are retained with
`eligible_for_training: false` and the hidden training files are removed. HM's
latest-file discovery therefore continues to select the most recent eligible
generation after failed, empty, incomplete, or normally crashed runs. A
normal EOS—including an intentional user stop after useful samples—preserves a
partial dataset as an eligible generation.

Together, the headerless HM files contain the policy inputs (tracked player
identities and boxes) and actions (fast/follower camera boxes), while the
sidecars preserve the media timeline and policy configuration needed to audit
or replay how they were produced. The game directory remains the source for
rink-mask features used by HM training.

## GPU-path and backpressure contract

The exporter copies only CPU-resident `NvDsObjectMeta`, frame timestamps, and
the native playtracker's small result structures. It never maps or reads an
`NvBufSurface`, performs a device-to-host pixel copy, or introduces a CPU image
conversion. File I/O runs on a dedicated writer thread behind a bounded queue
(2,048 complete frame samples by default). The streaming thread never waits for
disk I/O; if the queue fills it drops and counts a complete metadata sample.
