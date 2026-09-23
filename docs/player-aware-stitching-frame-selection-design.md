# Player-aware stitching reference frames

Status: implemented after approval by two independent xhigh-reasoning reviewers. The first-delivery scope below is authoritative; later extensions are explicitly deferred. Implementation validation is recorded below; PR review rounds are recorded in the PR.

## First delivery: automatic selection in Stitching Experiments

This section defines the first implementation.
The delivered feature includes automatic scanning and selection, not just manual
timestamp entry. The option is off by default. Normal Program startup is unchanged.

The user enables **Prefer player-rich frames** when adding experiment candidates
and chooses a bounded search duration. Each setting combination has an ordinary
baseline row and a player-selected row, both available for moving comparison and
existing explicit promotion. The anchor still occupies the first of the requested
frame slots. A one-frame experiment needs no search and is identified as an ordinary
anchor-only candidate. Near/far coverage is reported as apparent player size bands,
not measured distance. This feature nominates frames; it does not guarantee that
the resulting control points belong to players or that alignment improves.

The user's clarified requirement is to reuse Program's existing ice-mask pruning
algorithm. Sampled detections are acceptable; temporal tracking is not mandatory.
The analysis therefore scores only people surviving `ds-fieldmask`, including refs,
and does not count unfiltered spectators or use independently invented mask rules.

### Process and state ownership

1. Create and calibrate the ordinary private baseline through the existing runner.
   Baselines associated with automatic candidates also prepare their rink mask via
   an explicit calibration-only-with-ice-mask flavor (no inference or tracking).
   This uses the existing cancellable one-pass mask-generation path. Call `CompleteStitchingExperimentWorkspace()` only after its process session
   and helpers have stopped. Completion validates the required rink mask and marks
   it complete instead of omitted. Keep this baseline available throughout.
2. For the associated player candidate, run a separate bounded analysis process on
   the baseline: `decode/batch -> hmstitcher -> primary-gie -> ds-fieldmask -> FAKE`.
3. The analysis process reads a locked, validated baseline artifact snapshot,
   checks its generation against output metadata, and records only paired source
   identities, rink-filtered person boxes, and bounded coverage statistics. It writes a versioned
   report atomically only after successful pipeline shutdown. Cancellation, errors,
   missing inference, or changed geometry must not produce a successful report.
4. After analysis and helper shutdown, the backend validates the report and freezes
   its selected pair plan in a separate candidate's config. It verifies source and
   camera/synchronization context. Candidate calibration replays from the same anchor
   and captures the exact selected pairs using the existing GPU snapshot path.
5. Ordinary feature matching, per-pair control-point selection with additive pooling, Hugin/OpenCV solving,
   seam creation, and transactional publication run unchanged. Only a successful
   completed candidate becomes Ready. The user compares baseline/candidate and
   promotes through the existing transaction.

The dialog owns phase transitions; the runner owns each pipeline and metadata probe.
Use its existing process tokens, shutdown tickets and quarantine behavior in every
phase. A late completion cannot advance a different candidate. Baseline dependencies
must remain valid when queue rows are removed or duplicate settings are suppressed.
Limit the total queue including automatically added baselines to 64 rows.

No Program recording, rewind change, permanent feedback loop, new Python runtime,
or hm-cupano dependency change is needed. The scan uses existing stitched detector
inputs. A poor baseline seam can duplicate/suppress detections, which is a stated
experimental limitation; it never supplies synthetic raw-camera correspondences.

### Exact frame and source contract

Add a small `StitchingFramePairMeta` user payload in `src/libs/common/`. Before
`hmstitcher` discards either input frame's metadata, preserve both decoded source
IDs, integer source PTS in nanoseconds, physical chapter URIs, decoder sequences,
and the pair timeline PTS. The existing stitched-output metadata supplies geometry
identity. Copy/release callbacks must work across the CLI/plugin shared-library
boundary. Metadata contains no video pixels.

Canonicalize local physical paths when building the report so different private
symlinks resolve to the same media. Record a stable source stat signature including
size and modification time, plus the decoded origin and effective synchronization
and camera context. Decoder sequence is diagnostic, since new graphs can restart
it; exact canonical source URI plus integer source PTS for both cameras is the
authoritative pair identity. Reject nonlocal/unseekable sources in this mode.

One shared plan selector serves both runtime-size capture and `process()` capture.
For each selected pair, capture only an exact match; a skipped/mismatched pair or
EOS before the complete plan is an error, never permission to take the next frame.
The first planned pair must equal the anchor actually decoded on replay. Validate
the frozen sources/context before replay and again before candidate publication.
Plan bounds, duplicate identities, ordering, overflow, and unknown schema versions
are validated before decoding. The plan has at most 16 pairs.

### Overlap and selection contract

Compute shared visibility from both cameras' placed X/Y remap validity, not central
strips, rectangular extents, or seam colors. A valid remap pixel has both coordinates
inside its camera dimensions and neither coordinate equal to the unmapped sentinel.
Honor TIFF placement and the effective capped canvas scale. Read a single locked
artifact snapshot and check its generation before report completion.

Preserve the baseline's post-stitch rotation. Transform the CPU overlap mask into
exact output coordinates with the same inverse affine coefficients and fixed-canvas
center `(width - 1)/2, (height - 1)/2` as `apply_post_stitch_rotation()`; use
conservative validity at interpolated boundaries. Reject a runtime canvas mismatch.
Keep the CPU scoring mask bounded; reading calibration mapping artifacts is not a
video surface readback.

Before analysis starts, require both validated baseline mapping artifacts and a
current generation-bound rink mask. Analysis sets `calibrate-field-mask=0` so mask
work cannot activate calibration completion/rewind while scanning. The existing
fieldmask plugin gets a new opt-in `require-existing-mask` property, default false:
when true, an empty path, invalid/missing/mismatched mask or superseded authority
fails instead of regenerating or skipping pruning. Ordinary Program semantics stay
unchanged. Preserve Program's configured mask adjustments. All mask generation
occurs during the explicit bootstrap phase, whose existing bounded readback and
cancellation are measured separately from the scan's zero video readbacks.

At the post-fieldmask probe, require `bInferDone`, a nonempty `CV_8UC1`
`FieldMaskPayload.mask()` with the exact frame dimensions, and a payload revision
matching `StitchedOutputGenerationPayload.generation() + ":" + authorization_id()`.
Missing/mismatched payloads fail the scan: the plugin can return OK without pruning
when no mask is configured, publication is superseded, or dimensions differ. Only
after these checks may an empty object list mean a valid empty-ice observation.
The graph must use the standard fieldmask plugin and a nonempty configured mask
path; no fallback to raw detections is permitted.

Use class 0 (people, including players and referees) from the configured primary
detector AFTER the existing `ds-fieldmask` plugin has pruned object metadata, with
finite clipped boxes and capped per-frame detections. This is the exact same
filtering stage Program uses before tracking; never substitute a simpler feet-in-ice
test or score the pre-mask `DetectionSnapshotMeta`. Persistent tracking is not
required, per the user's clarification, so sparse sampling remains valid. Count a box only where it has
meaningful intersection with actual overlap. Score occupied overlap cells and
apparent-size bands, capping each person's contribution so large boxes and crowds
cannot overwhelm spatial diversity. Choose the anchor plus later frames greedily
by marginal coverage, then quality, then earlier time for deterministic ties, with
a minimum time separation. Do not invent detections to fill an absent size band.

Bound duration (maximum 300 seconds), metadata observations (maximum 1200), and
per-frame detections (maximum 256). The experiment UI starts with a 60-second search
and 500 ms scoring interval. These are experiment controls, not new Program defaults.
Inference cadence uses an explicit scan-only pre-inference metadata gate: keep
the first pair and pairs at the requested time spacing, dropping other *analysis*
buffers before nvinfer. Support/reject batch shapes explicitly; never apply the
gate to ordinary calibration or Program. Check `bInferDone` after inference;
skipped inference is not an empty-ice observation. Require inference interval zero
in this graph because the gate already schedules inference.

An empty/short scan or insufficient separated player-rich frames reports an
unavailable player candidate with diagnostics; the baseline stays Ready. Actual
source/model/inference failures remain errors. Do not silently duplicate pairs or
reduce the requested count. The scan may finish at clean EOS before its duration.

### Configuration and provenance

The CLI exposes a distinct analysis/report option and explicit duration/sampling
parameters, usable by the dialog and for debugging. Apply the analysis graph helper
both during asset discovery and before full configuration/TensorRT cache preparation.
It preserves primary GIE and `ds-fieldmask`, and suppresses secondary inference,
crop, tracker, camera play tracking, telemetry, encode/network sinks and audio. Require a preexisting valid baseline;
analysis must not silently trigger calibration.

Persist the frozen plan and selection diagnostics as generated
`stitching.calibration_frame_selection` state, including schema, selection algorithm,
source bindings, baseline generation, detector identity/configuration, exact selected
pairs, and actual coverage. Also bind the exact rink-mask content/revision, output
rotation and effective canonical/native fieldmask adjustments. Hashing the actual
mask binds the segmentation result used for pruning; geometry alone does not identify filtering. Absence retains current frame selection. This is generated
calibration input/provenance, not a new independent set of canonical user defaults;
the shared baseline remains byte-for-byte unchanged.

Bind a deterministic selection fingerprint into `StitchingBackendChoices`, worker
generation claims, and canvas provenance. New ordinary experiment workspaces clear
any inherited plan. Once promoted, exact pairs persist across subsequent solve-parameter changes, including lens/FOV,
matcher, resolution, projection, crop and leveling. Replay validates media, ordered chapters, synchronization and the
saved anchor; the original scoring geometry remains provenance rather than a restriction on later solves. A conflicting
reference-time edit fails without changing the plan. Only an explicit replacement (including changing the requested
frame count or promoting another candidate) changes the frame policy. Unavailable or mismatched selected pairs fail;
there is no fallback to ordinary frames. Promotion copies the inline provenance through the existing
config allowlist and publishes no temporary paths. Avoid making normal loading of
already-generated artifacts depend on the original media's current location; exact
source validation applies when re-extracting or creating a selected-frame generation.

Record the representative pair selected for the solved candidate and per-frame
accepted match counts for diagnostics. Neural matcher `accepted` means confidence
and bounds checks, not verified player identity or a geometric inlier guarantee.
Do not change the current selector or add player-point weighting in this delivery.

### Inspecting the chosen frames

**Inspect selected frames** is available after a candidate's plan is frozen, including
when its later solve fails. It lists ordered left/right physical files, exact integer
source timestamps and sequence diagnostics, anchor/timeline positions, eligible
people, apparent-size counts, quality and a labeled 16×9 coverage diagram in the
baseline stitched canvas. It rechecks source bindings when opened.

Side-by-side stills come from the CPU images already loaded for feature matching,
resized to at most 1024 pixels on the longest edge and stored under the candidate's
`player-frame-inspection/<selection fingerprint>/` directory. The UI only accepts
bounded regular images for that fingerprint and shows an explicit unavailable
state until extraction reaches those frames. These private diagnostics add no
analysis or Program video readback and are not promoted as stitching artifacts.
The coverage diagram is separate from raw-camera stills: scoring boxes belong to
the baseline stitched coordinates and are not misrepresented as raw-camera boxes.
Exact identities and scores remain in the promoted inline plan.

### Implementation sequence and file ownership

1. `src/libs/stitching/PlayerFrameSelection.*`: pure bounded scoring/selection,
   strict report/plan parsing, source/context bindings, deterministic fingerprints,
   and the shared exact-replay selector. Add colocated meaningful tests.
2. `src/libs/common/StitchingFramePairMeta.*` and `stitcher/`: metadata preservation
   plus exact plan consumption in both capture paths; retain default behavior.
3. `src/libs/stitching/PlayerFrameOverlap.*`: bounded remap validity loader/scoring
   mask with placement/scale tests. Reuse existing artifact snapshot validation.
4. `src/apps/hstream-cli/`: analysis graph policy, CLI validation, pre/post-inference
   metadata gates, a post-fieldmask scoring probe and success-only report finalization. Verify assets and TensorRT
   preparation use the same mode as graph assembly.
5. `gst-fieldmask`: optional require-existing-mask behavior and matching tests,
   preserving default behavior; bootstrap completion validates required masks.
6. `GameConfig`, `ConfigureStitching`, `HuginProject`, and canvas provenance: plan
   identity/ownership, source checks at publication, and match diagnostics. Extend
   existing generation mechanisms rather than adding a parallel publication scheme.
7. `StitchingExperimentBackend.*` and dialog: optional controls, baseline/automatic
   queue dependencies, scan/solve phases, diagnostics, cancellation, and promotion.
8. Update `AGENTS.md`, experiment documentation, and this status/validation record.

### Validation and review gates

Before implementation, both xhigh reviewers must accept this revised plan. Review
changes until no necessary design fixes remain. After implementation, build all
x86 targets and validate the Jetson path on `stubby` (and native SBSA if available),
then open a ready-for-review PR. Have two xhigh agents independently review the PR;
fix necessary findings and repeat with two reviewers until no necessary fixes remain.
Record rounds, validation and any real environment limitations in the PR.

Focused checks cover deterministic selection, empty/size-biased/crowded scenes,
invalid metadata and limits; masks with holes/offsets/scaling; paired metadata
copying and right-frame reuse; exact replay across chapters/nonzero starts/unequal
offsets; generation/source/plan changes; clean short EOS versus errors; phase
cancellation/stale completion; asset mode and isolated fake sinks; baseline and
promotion compatibility. Run a real GPU experiment that selects later frames and
replays a candidate, inspect moving baseline/candidate previews, and measure scan
memory/readback behavior. Report observed quality without assuming improvement.
Include a spectator/player fixture that exercises the real fieldmask implementation,
including its upper/lower-ice rules, and verifies only surviving people contribute
to scan scores. Test missing, stale and size-mismatched FieldMaskPayload rejection.

### Design review record

- Round 1, two xhigh agents: original broad proposal was not implementation-ready.
  Both accepted the smaller stitched-scan Experiments architecture conditionally.
  Necessary fixes were paired source provenance, exact replay, actual remap overlap,
  rotation coordinates, early inference provisioning, bounded real sampling,
  preserved baseline ownership, and generation-bound plan promotion. This plan
  incorporates those fixes and explicitly defers raw detection and player weighting.
- User clarification after round 1: people must pass the same ice-mask pruning used
  before Program tracking. Sampled detections are allowed; tracking is optional.
  The scan now includes the existing fieldmask stage and validates its generation
  payload before scoring. Both reviewers supplied the exact filtering/probe contract.
- Round 2: geometry review accepted after adding mask/filter provenance. Runtime
  review identified mask generation during scanning as a rewind/lifecycle hazard.
  The plan now prepares masks during bootstrap, requires existing masks during
  scanning, preserves baseline rotation, and rotates overlap coordinates instead.
- Final design review: both xhigh reviewers approved with no necessary fixes.
  Scan launches must clear calibration-pending/reconfigure environment flags and
  assert that no calibration-required context remains.
- User inspection requirement: added ordered pair identities, timestamps, scores,
  coverage diagram and bounded stills reused from the calibration image loads.
  The UI/geometry agents reviewed the representation to avoid incorrect overlays
  of stitched detection boxes on raw-camera images.

## Geometry limits and later work

The first delivery selects action frames and retains current matching/solving. It
makes no claim that neural accepted matches are player points or geometric inliers.
A poor bootstrap seam can duplicate/suppress people and mask errors can misclassify
spectators, just as in Program; diagnostics and moving comparison expose that limit.
Separated cameras have depth-dependent parallax, and moving players reveal timing,
blur and rolling-shutter errors. A fixed mapping cannot necessarily align near and
far players simultaneously. Dynamic seams or local warps would be a separate larger
hm-cupano project.

Future work can evaluate raw-camera detection with equivalent ice filtering,
balanced player/background point quotas, matching on player crops, tracked-motion
coverage with full-rate processing, or automatic startup selection. First measure
ordinary versus selected-frame calibration at equal frame/point budgets on held-out
moving passages, reporting apparent-size coverage and background/action alignment
separately. Preserve panorama quality and the existing complete artifact set.

## Implementation validation

PR review round 1 used two independent xhigh reviewers. They found three necessary
fixes: preserve the plan fingerprint from capture through publication even without
a UI generation owner; freeze inherited baseline camera settings before candidate
handoff; and reconcile ordinary reference-time/frame-count controls with a promoted plan. Each fix includes a regression.
The final persistence policy rejects reference-only changes and preserves the plan across solve-parameter edits;
changing the frame count explicitly requests replacement. Round 2 found one additional handoff edge case:
a baseline can omit a reference time inherited from user settings while the new
candidate still stores it explicitly. Handoff now preserves the baseline’s stored
form, including absence, after separately validating the actual decode anchor.
The inherited-time regression and full platform builds passed; round 2’s runtime
review found no necessary fixes. Round 3 found a promotion mismatch between absolute/aliased input paths and
the candidate’s relative paths; promotion now normalizes only proven-equivalent sources. A user run on `mini` additionally exposed a cold-start
timing defect: the first detector engine build was followed by a queried absolute
seek position being treated as completed scan time, so the scan stopped at zero
inferred frames. Scan completion must use only samples that finish inference and
rink filtering, including the duration boundary; engine preparation and upstream
position queries must not consume the scan window.

- Full x86 build: `bazelisk build --config=opt --cpu=k8 --config=blackwell --jobs=4 --spawn_strategy=local //...`.
  Local spawning avoided an intermittent nvcc temporary-file collision in sandboxed parallel compilation.
- All 17 focused x86 tests plus the focused main-UI regression suite pass: selection, overlap, detector identity, scan mask
  validation, observed-frame timing, graph policy, Configurator persistence, experiment backend/dialog,
  paired metadata, three fieldmask fixtures, one-pass replay, GameConfig,
  ConfigureStitching and Hugin provenance.
- Full native Jetson build on `stubby` passes with `--config=jetson --jobs=6`;
  all 15 compatible focused tests pass. The two Qt UI tests are incompatible with
  that platform. A separate output base and symlink prefix isolate remote builds.
  Native non-Jetson SBSA execution was not available.
- A disposable real 3840×2160 hockey recording passed the complete GPU experiment:
  baseline plus rink mask, 10-second scan at 500 ms, exact two-pair replay and solve,
  both moving previews, inspection and transactional promotion. The scan observed
  20 samples and selected the anchor plus +5.005 seconds. Those pairs contained
  9 and 11 eligible people in the overlap; the later pair included far/middle/near
  counts of 1/7/3. The original game's configuration remained unchanged.
- Visual inspection caught and fixed 16-bit stills saturating in JPEG thumbnails.
  The repeated workflow passed with clear camera images and an added tonal-content
  assertion. Xwayland root screenshots of the GPU preview were blank; renderer
  first-frame acknowledgments and five-second clock-pacing assertions passed.
- Real SIGINT cancellation returned an error without a selection report. The scan
  requires an observed inference boundary or natural EOS before graceful shutdown;
  an interruption latch prevents shutdown-generated EOS from authorizing a report.
- Nsight Systems profiling of the final 10-second scan observed 21 D2H transfers,
  each 0.719 MB (detector outputs: 20 samples plus one time-limit boundary frame),
  and no video-sized D2H transfers. Observed process peaks under profiling were
  3880 MiB GPU memory and 2843 MiB RSS; the profile command completed in 9.24 seconds
  including profiler export. These measurements describe one fixture, not a general
  performance guarantee. The scanner and inspector add no video-surface readback.
- Native Jetson testing exposed an existing absent-versus-null resolution provenance
  bug: restoring an absent game override installed null over the inherited user
  setting. The narrow fix preserves absence, with a regression. Test fixtures also
  state legacy native-resolution assumptions explicitly across platforms.

- Cold-cache regression on `mini` with the user’s 8K footage at 00:09:42:
  the old runner reproduced zero observations; the fixed runner built a fresh
  detector engine, then processed 120 samples and selected four pairs. Both tests
  used isolated caches and a private copy of the baseline, preserving the open UI
  session and the original game. Full x86/Jetson builds and focused tests passed
  after the review and timing fixes.

A subsequent main Program run on `mini` exposed an NFS snapshot validation defect: the private artifact snapshot
did not contain the selected-plan configuration. Validation now checks snapshot artifacts against the source game
configuration, retaining rejection of a replaced or cleared plan. Promotion also binds crop-review state to the
selected candidate’s actual geometry. Main Program startup after promotion must be included in integration
validation; calibration-only previews do not exercise this path.

The broader reuse audit found and corrected two UI false-change paths: inherited projection parameters were compared
against built-in defaults on Play, and two-decimal spin boxes rounded stored geometry on load. The UI now compares
effective inherited values and retains calibrated precision until an actual edit. Logs identify changed inputs and
the actual rebuild scope. Intentional canvas changes can still rebuild the full solve with the same selected pairs;
there is no separate map-only resume phase today. A byte-identical PTO restored with a newer timestamp can also
invalidate older maps. That conservative timestamp check remains: the current generation sidecar can adopt changed
content and cannot alone prove that the maps were generated from it. Bypassing the check requires stronger persisted
validity evidence.

The audit also found dangling pointers to loop-local YAML handles in the shared `get_node` and `has_node` helpers.
AddressSanitizer reproduced stack-use-after-scope in both; traversal now retains a live handle and uses `reset` to
avoid mutating aliased configuration. Regression coverage checks nested/sequence paths, absent and null values,
and unchanged source documents. This was a verified configuration-read defect, not the established cause of the
reported NFS failure.

The expanded real GPU workflow passed baseline generation, player selection, exact-pair solve, both previews,
inspection, promotion, automatic dialog closure and then five seconds of main Program video. Main Program reused
the exact canvas provenance without new captures or feature matching. A separate NFS run on `mini` also processed
video successfully with an identical saved frame plan and canvas provenance. Scoreboard overlay was disabled in these
headless Program checks to avoid an unrelated manual scoreboard-selection prompt.

## Follow-up: shared frame inputs and per-row inspection

Design approved by two xhigh reviewers and implemented. Durable game-local history and complete input bundles are
described in [the persistence follow-up](persistent-stitching-frame-cache-design.md).

The frame-count selection belongs to the experiment's input state, independently of control-point limits and other
solve options. A fresh player-enabled count gets one ordinary baseline and one scan. The first Players candidate
freezes the resulting plan; every later option with that count reuses it and performs only its own matching and
calibration. A different count can create another selection. Conflicting reference times for an established count
fail rather than silently creating a second set. The first scan duration remains authoritative for its count.

A plan already promoted into the main game is inherited by same-count experiments, including when the preference
checkbox is off. Preserve its fingerprint, source bindings and reference-time spelling/absence. A conflicting source
or reference fails; changing the count explicitly permits a fresh set. A frozen plan remains reusable after its
owner's subsequent matching/solve fails, provided the worker has stopped safely. Quarantined workspaces are never
reused. Removing an input-owner row removes or safely reassigns its dependencies before the batch starts.

Every row's inspector shows that row's actual input frames. The initial ordinary baseline can have different frames
from its derived Players row; neither borrows the other's thumbnails. Ordinary input inspection retains bounded
thumbnails and source metadata from the CPU stills already required for matching, with ownership tied to that
candidate's generation. This adds no video-surface readback and cannot affect steady-state playback. Missing or
unavailable metadata/images must be explicit; never substitute nearby frames. Player counts/coverage are shown only
when a player scan produced them.

Validation covers one scan across control-point/rotation variants, separate selections for different counts,
same-count inheritance after reopening, owner solve failure, source/reference rejection, and inspecting each row's
own images. The moving GPU workflow must still promote an existing generation and play it without recapture.
