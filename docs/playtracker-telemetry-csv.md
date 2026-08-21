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
pre-existing HM dataset.

The three training files are headerless, as expected by `../hm`:

| File | Columns | Meaning |
| --- | --- | --- |
| `tracking[-N].csv` | `Frame,ID,BBox_X,BBox_Y,BBox_W,BBox_H,Scores,Labels,Visibility,JerseyInfo,ActionLabel,ActionScore,ActionIndex` | Current 13-column `TrackingDataFrame` schema. HStream supplies tracked player ID, score, class, and TLWH; unavailable jersey/action annotations use HM's neutral values. |
| `camera[-N].csv` | `Frame,BBox_X,BBox_Y,BBox_W,BBox_H` | Follower/Program camera action in TLWH coordinates. |
| `camera_fast[-N].csv` | `Frame,BBox_X,BBox_Y,BBox_W,BBox_H` | Fast camera-policy action in TLWH coordinates. For a one-box policy it matches `camera.csv`. |

`Frame` is a monotonic, one-based export sample ID shared by all three files.
This keeps the files joinable across source seeks and chapter transitions. HM's
`CameraPanZoomGPTIterableDataset`, `CameraPanZoomDataset`, `camgpt_train`, and
DriveGPT training mode can consume these files without a schema conversion.
As in HM's own saver, frames with no player rows are absent from
`tracking.csv`; HM training naturally excludes them at the tracking/camera
intersection.

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
  paths, completion status, and loss counters. `completed: false` means the run
  did not reach a graceful exporter stop. Any nonzero `dropped_samples` means
  the bounded queue could not keep up; each drop is a complete sample, so the
  HM input/action files never become cross-frame misaligned.

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
