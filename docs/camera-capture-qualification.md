# Camera comparison test driver

`//src/apps/hstream-ui:camera_experiment_dialog_test` has optional qualification
arguments. They change the test driver only; production rendering, dialog APIs,
and replay controls are unchanged. Use a private compositor and a dedicated
process owner. The owner must bound the entire checkpoint, including synchronous
internal capture, and retire every descendant after a timeout. Never use the
physical desktop, stop the application with SIGSTOP, or inject replacement pixels.

## Matched control context

Pass `--control-snapshot=/absolute/context.json` to `--e2e`. The bounded JSON format
uses an array so duplicate control identifiers cannot silently overwrite values:

```json
{
  "schema": 1,
  "playback_start_ms": 0,
  "controls": [
    ["Max_Speed_X_x10", 15],
    ["Apply_To_Fast_Box", 0],
    ["Apply_To_Follower_Box", 1]
  ]
}
```

This example is partial and does not qualify the whole initial state. A matched
comparison supplies every applicable Tracking, Motion and Players identifier
from the main window's `cameraControlValue` map, including both target choices.
Values use raw native units: `15` for `Max_Speed_X_x10` displays `1.5`. Unknown,
duplicate, nonnumeric and nonfinite entries fail. Playback start is an integer
number of milliseconds; a supplied positional Playback start must agree.

`control-context-initial.json` records actual constructor values, divisors,
enabled/checked states and whether the supplied map was complete. Values that
cannot be represented without changing their raw value fail. The driver then
unchecks runtime overrides without clearing their inherited values, selects Fast
off/Follower on, and applies only speed X 1.5 and acceleration X 0.2.
`control-context-trial.json` records that state; the final real Save verifies its
exact tuning. The ordinary no-snapshot run still uses its empty constructor map
and must not establish production main-window default parity.

## External capture handshake

Create an empty directory owned by the test user with mode `0700`, then pass
`--capture-handshake-dir=/absolute/handshake` to `--e2e`. Reusing a nonempty
directory fails. The driver first saves `capture-reference-trial.yaml` through
the real dialog, separately from its later Save/Cancel/Close cases. The driver
first pauses through the ordinary playback button, waits for a confirmed frame
and Save readiness, saves, then resumes and requires new confirmed frame progress
before its playing captures. `capture-reference-playback.json` records that
transition. The trial's hash and exact sample/PTS rows bind capture evidence.
Recorded, baseline and trial views
share sample identity; trial ROI/overlay fields do not describe the other views.

Each checkpoint uses a random run token, increasing sequence and unique phase:

1. `begin-000001.json` arms the outer owner's checkpoint watchdog before internal
   capture. The owner must also have a whole-run deadline for preparation and Save.
2. `request-000001.json` publishes the internal PNG path, geometry, controls,
   selected source/view, requested index and Poll-derived confirmed index/label,
   immutable trial sample/PTS, and synchronous capture duration.
3. The observer captures two real compositor outputs, records timestamps and
   geometry, makes the artifacts durable, then atomically publishes
   `ack-000001.json` containing exactly the request's identity fields:

   ```json
   {"schema":1,"run_token":"copied-from-request","sequence":1,"phase":"paused-normal"}
   ```

4. The driver waits at most five seconds after request publication, processing
   ordinary Qt events and heartbeat timers. An earlier sequence cannot acknowledge
   the current request. Wrong identity, malformed/oversized JSON, changed directory
   identity or changed checkpoint state fails. `result-000001.json` records the
   outcome. The observer must not acknowledge an image it has not captured.

Paused checkpoints require the requested and Poll-confirmed indices to agree and
remain stable with geometry, source, controls and view. These are UI/Poll evidence,
not an invented native GPU-generation receipt. Playing geometry checkpoints keep
geometry/source/view/control state stable while recording frame observations
before/after internal capture and after acknowledgment. They describe a bounded
frame interval; the externally captured image needs independent frame analysis.

Visual timeline seeks include both an increasing seek sequence and the requested
frame in their phase names, including repeated seeks to the same frame. Confirmed
indices are parsed from the frame segment of the complete native Sample label;
covered or malformed labels never establish readiness. Ordinary runs have no
reference trial and do not read reference rows or compare their final Save to one.

Named normal, resized, restored, preview-maximized and window-maximized phases
retain separate PNGs and geometry. Paused visibility probes are separately named
in ordinary mode and are not external-capture acknowledgments. Handshake mode
captures once per paused phase and fails on a black frame, so every synchronous
readback is covered by its pre-capture watchdog marker. With `--overlay-frame=N`, handshake
mode also uses that fixed frame for paused geometry. All-layer, no-layer and
individual layer captures retain the actual plot captions and layer flags.
Raw inverted Weston captures remain
unchanged; any lossless vertical-flip review copies must be labeled explicitly.
Acknowledgment proves capture completion, not pixel correctness or whole C2 parity.

## Close during Save

`--close-during-save` runs a separate final Close probe after successful Save and
Stop/reopen, before changing the source/range. This option leaves stale-source
Save invalidation unrun; use the ordinary invocation for that existing case.
It is also accepted by the CPU `--save-trial` workflow.

The probe distinguishes admitted UI state, an outstanding UI future, observed
uncommitted temporary output, and an already committed result. It calls ordinary
Reject, checks visible closing while the future is outstanding, waits for normal
closure, and rejects partial output or a late reopen. A committed-file-wins race
is permitted only when the bytes equal the prior verified Save. The `.close.json`
receipt records which boundary was actually observed.

Temporary output is created after YAML serialization. Neither its existence nor
"Cancel saving" text proves that the serializer was running when Close occurred.
The active serialization boundary is explicitly **unrun** without independent
worker/barrier evidence. No production barrier is added, and the early reference
Save cannot substitute for cancellation/Close coverage.

## Validation scope

The default CPU suite includes the standalone `--capture-driver-test` regressions:
acknowledgment success, stale/wrong identities, malformed and oversized records,
timeout, changing checkpoint state, live event-loop timers, and invalid control
snapshots. Existing Camera CPU/Save/replay tests remain required. Build the x86
target before live qualification. Qt UI targets are explicitly incompatible with
the Jetson platform; report that build limitation rather than a Jetson pass.
When the platform is exactly `offscreen`, this test binary supplies a private
1920×1400/DPR1 virtual screen so its 480px preview and 390px pane fit. Explicit
caller platform configurations remain authoritative. The CPU geometry case
checks actual plot/pane/preview bounds through resize, maximize and restore.

For a paired visual review, match initial controls, actual DPR, outer geometry,
source/view/frame, layer flags and scroll positions. The seven selected seeks in
the retained dense fixture expose burned labels only for frames 0 and 3; the
other five crop the label out in both Recorded and trial views. Historical rink
mask, active serialization Cancel/Close and physical GNOME output each need their
own evidence. Do not infer them from handshake success or a no-mask recording.
