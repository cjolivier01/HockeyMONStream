Camera experiment replay
========================

`ReplaySession::Prepare` reads a completed telemetry generation on a worker
thread. `start_seconds` is relative to its first frame-index PTS, and the out
sample is exclusive. The range must fit within one source/seek/reset/geometry
segment. Sample IDs remain sample IDs; numeric gaps never manufacture video
frames. Empty input frames preserve production's initialization and native
forward behavior.

New generations restore the versioned native checkpoint immediately before the
selected sample, replay preceding recorded inputs as needed, and verify both
recorded camera trajectories. Exact native TLBR inputs avoid recovering right
and bottom from rounded CSV widths. Checkpoints contain native state, effective
configuration, wrapper `has_received_tracks`, and a separate base configuration
for absolute zoom and reset-to-default motion controls. Numeric text preserves
float precision. Input parsing and retained range size are bounded and honor
cancellation; no video pixels enter this library.

Legacy recordings require the historical arena explicitly and the archived
effective configuration (or an explicitly supplied original file). They replay
from the original initialization through recorded runtime/configuration events
and must reproduce both camera trajectories within a measured pixel tolerance.
Missing provenance or disagreement fails with an explanation. Legacy CSV
restoration is trajectory-verified, not bit-exact. Existing historical native
defaults may differ from this implementation, which measured verification will
reject rather than silently replacing historical state with current defaults.

The original camera, a recomputed baseline with configuration frozen at the
fork, and candidate trials are distinct. Every trial owns a fresh tracker
restored from the same immutable pre-frame checkpoint; recorded configuration
events after the fork cannot overwrite its overrides. Repeated trial calls can
run concurrently. A cancelled or failed call cannot mutate the session.

`SaveTrial` writes a YAML descriptor containing the checkpoint, base state,
recording manifest, exact selected inputs, trial overrides/results, and the
explicit uncropped-panorama PTS mapping and file identity. This descriptor is
separate from the original training telemetry. Media content binding is manual;
matching dimensions alone cannot verify the panorama depicts the recording.

Verification:

```
bazelisk test --config=opt --cpu=k8 //src/libs/playtracker_replay:playtracker_replay_test
bazel-bin/src/libs/playtracker_replay/playtracker_replay_test --recording /path/hstream_telemetry.json 10 20
bazel-bin/src/libs/playtracker_replay/playtracker_replay_test --recording /path/legacy/hstream_telemetry.json 10 20 0 0 3840 2160
bazel-bin/src/libs/playtracker_replay/playtracker_replay_test --make-fixture /tmp/camera-experiment-fixture
```

The fixture mode exercises the actual production exporter and native CPU
stepping/capture path. It writes a 30-second recording with player motion,
initial and later empty frames, runtime tuning, and a seek boundary. Its canvas
is 3840x2160; telemetry starts at PTS 2 seconds. It is synthetic metadata for
integration tests and UI development, not an actual hockey recording.
