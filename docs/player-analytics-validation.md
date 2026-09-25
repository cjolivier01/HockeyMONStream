# Player analytics validation

## Foundation (PR 1)

Base: `ad40b08e618445162211a1a4587c202586d7a27f`. Validation performed 2026-09-24.
This stage implements shared track colors, optional native ReID, and the CPU
contracts for the later inference stages. Pose, jersey, action, and their new GPU
compositor are not yet runtime features of this stage.

### Builds and tests

The x86 full build passed with:

```sh
bazelisk build --config=opt --cpu=k8 --config=blackwell --jobs=8 --sandbox_tmpfs_path=/tmp //...
bazelisk build --config=opt --cpu=k8 --config=blackwell --sandbox_tmpfs_path=/tmp \
  //src/apps/apps-common:hm_gpu_preview_test //src/libs/player_analytics:all \
  //src/libs/common:track_color_meta_test
```

The native Jetson full build also passed:

```sh
ssh -x stubby 'cd /home/colivier/src/hstream-player-analytics && bazelisk build --config=opt --config=jetson --jobs=4 --symlink_prefix=/tmp/hstream-player-stubby- //...'
```

The separate preview target is tagged `manual`; the all-target build does not
select it. Native `sm_120` is needed on this RTX 5090 because this machine's
CUDA toolchain PTX fallback is newer than its driver. A first parallel build hit
an nvcc temporary-file collision; an isolated sandbox `/tmp` resolved it.

Explicit execution passed for `track_colors_test`, `config_test`,
`model_contract_test`, `temporal_state_test`, `frame_meta_test`,
`tracker_reid_test`, `track_color_meta_test`, `preview_overlay_meta_test`,
`playtracker_baseline_defaults_test`, `plugin_property_yaml_parser_test`, and
`hm_gpu_preview_test`. The last test exercised the actual GPU/X11 renderer,
including matching Program/Stitched colors, resource release, transform gating,
and existing exception/lifecycle checks. Core metadata tests copy through
DeepStream into independent batch pools and exercise allocation failure.

Real-recording Stitched GPU preview runs passed with and without vpplaytracker.
Inspector snapshots confirmed the tracked preview tee exists and the redundant
`hmstitcher_preview_converter` branch is absent in both graphs.

Actual NVIDIA ReIdentificationNet engines loaded and processed 48 GPU frames
with 46 tracked outputs on x86 DeepStream 9.1/TensorRT 10.16 and Jetson
DeepStream 7.1/TensorRT 10.3. Generated runtime configuration was removed after
tracker destruction. Corrupt-engine rejection was also checked without allowing
an ONNX rebuild. These are compatibility/lifetime checks, not hockey identity
accuracy or reassociation benchmarks. See the
[preparation recipe](../src/libs/tracker_reid/README.md).

### Initial disabled-path comparison

A frozen master executable, own plugins, dependency libraries and configs were
copied before edits and content-hashed. Three alternating master/candidate runs
used a private copy of `gse-16a-short` calibration (13705×3919 canvas), the same
camera media, FAKE sink, 15 seconds of video, and the same detector configuration:

```sh
HM_GAME_DIR=/tmp/hstream-player-analytics-validation/games \
HM_OUTPUT_WORK_DIR=/tmp/hstream-player-analytics-validation/output \
  bazel-bin/src/apps/hstream-cli/hstream-cli \
  -c configs/ds_hockey_app_config.yaml --game-id=baseline \
  --enable-sources=URI-MULTIPLE --enable-sinks=FAKE \
  --options=stitching.control_point_resolution=native -t=15
```

| Warmed pair | Master final reported FPS | Candidate final reported FPS | Master/candidate peak GPU MiB |
| --- | ---: | ---: | --- |
| 1 | 63.55 | 63.41 | 10560 / 10560 |
| 2 | 63.15 | 63.03 | 10560 / 10560 |

The warmed final-sample difference is about -0.21%, below the design's 2%
investigation threshold. Both series slowly decreased across the run order;
this small difference does not establish a causal regression. The first candidate
run included launch-cache preparation, one fewer periodic FPS sample, and an
8 MiB larger peak. Do not compare startup wall times between the installed-style
frozen bundle and the initial Bazel launch cache.

The final PR4 audit found different detector-engine hashes in the historical
master/PR1/PR3 caches. These initial observations do not establish a controlled
performance bound; the explicitly pinned, hash-verified PR4 comparisons below
supersede them.

All analytics model work is absent from this stage's normal graph. Color state
is updated only for requested player drawing; off ReID returns before filesystem
inspection. Full inference/rendering comparisons, allocation traces, and drawing
cost measurements remain required for later stack stages.

Local evidence is retained under `/tmp/hstream-player-analytics-validation/`
(`pr1-comparison.json`, per-run logs, frozen-master `provenance.json`),
`/tmp/hstream-player-pr1-{full-build,tests-final}.log`, and native ReID logs
`/tmp/hstream-reid-native-{x86,stubby}.log` on the corresponding hosts.

## Review history

The design was iterated with three xhigh agents, including a researcher fetching
primary model sources. Two independent xhigh reviewers then reviewed the
implementation plan in two rounds. Resolved requirements included causal action
resampling, color recovery after temporary overflow, pre-crop preview ownership,
strict model contracts, and exact preprocessing validation. PR implementation
review rounds are recorded here as they complete.

PR1 implementation round 1 reviewed `c6bd5236` independently for pipeline and
runtime/core correctness. One required fix: deriving Program color demand from
the root drawing flag ignored later cropper property overrides. Demand now reads
the created cropper's final private configuration once, matching its alias,
last-token and integer parsing behavior. The regression test exercises 11 actual
cropper configurations across four producer combinations, including native-tracker
fallback ownership and reuse. It passes on x86 and native Jetson. Full builds on
both platforms pass after the fix, including the explicitly built x86 GPU preview
target. Logs: `/tmp/hstream-player-pr1-round2-{build,test}.log` on the corresponding
hosts; x86 regression detail is in `hstream-player-pr1-color-demand-test.log`.
The independent runtime/core reviewer found no required fixes in round 1.

PR1 implementation round 2 reviewed `4c6484db` with two independent xhigh
reviewers. Both identified the symmetric play-tracker case: public replacement
of `plugin-private-config` could discard resolved color demand. The builder now
appends the authoritative Program request after all user property overrides,
preserving other private settings. Eight additional actual play-tracker builder
cases cover on/off demand with private and public overrides. The expanded test
passes on x86 and native Jetson; full builds pass on both platforms after this fix.
No other necessary findings were reported in round 2.

Native Jetson frozen-master/default-candidate playback now also completes with
positive output FPS. Two alternating pairs use the same private 7135×2634 canvas,
4K camera sources, FP32 detector, FAKE sink and five seconds of video. Both use
`pipeline.hmstitcher.properties.high-bit-depth=0` because this frozen DS7.1 master
cannot negotiate the high-bit-depth stitched path. Periodic FPS varies from
7.54 to 9.67 across runs; both variants average about 8.4 FPS. These short runs
establish the fixture and successful playback, not a precise performance bound.
The first candidate startup includes its Bazel launch cache. Longer controlled
runs and model/render comparisons remain part of PR4. Remote evidence:
`/tmp/hstream-player-analytics-validation/pr1-jetson-comparison.json` and its logs.

PR1 implementation round 3 reviewed `b519da7c`. Both reviewers caught a
property-ordering regression in the second fix: writing the entire private
configuration after public properties advanced the plugin's precedence sequence,
so unrelated typed acceleration/rotation overrides could lose at startup. The
corrected builder augments each private-config value before its original setter
runs and preserves the original public-property order. The regression now checks
the actual plugin's startup precedence markers as well as color demand, including
drawing disabled. The previous round's correction is superseded by this ordering-
preserving version. Round-4 builds/tests are recorded in
`/tmp/hstream-player-pr1-round4-{build,test}.log` on each host.

PR1 implementation round 4: both independent xhigh reviewers report no necessary
fixes at `4d450dbb`. The final fresh x86 build exposed a missing CUDA library
search path in the new test-only header target; it now mirrors the existing
plugin's platform-specific NPP search paths. Production behavior is unchanged by
that build correction. The regression test and complete x86/Jetson builds pass
with the corrected test target. Final x86 evidence is
`/tmp/hstream-player-pr1-round4-isolated-{test,build}.log`, using a dedicated
`--output_base=/home/colivier/.bazel-player-analytics` because unrelated worktrees
were replacing the default shared outputs. Use this output base for subsequent
local builds and runtime verification.


## Native pose and offline model preparation (PR 2)

This stage adds the optional `hmplayeranalytics` element, native TensorRT runtime,
GPU pose crops/decoding and offline preparation for the three supported profiles.
The all-off graph omits the element. A manually instantiated disabled element also
passes with zero intercepted CUDA calls and without accessing a video surface or
model bundle. Live graph inspection plus file-access tracing confirms ordinary
playback omits analytics and does not inspect an inaccessible configured bundle.
Calibration-only playback suppresses even explicitly enabled, invalid analytics
settings and completes both source EOS paths.

The complete x86 build uses the isolated output base above and the coherent
TensorRT 10.16 SDK selected with
`HSTREAM_PLAYER_TENSORRT_SDK_ROOT=$HOME/.cache/hstream/player-sdk/trt10.16/usr`.
The native Jetson build uses its TensorRT 10.3 SDK. The x86-only manual preview
target is explicitly built on x86; it is incompatible with the Jetson platform.
Logs are `/tmp/hstream-player-pr2-{final-build,full-build}.log` on their respective
hosts. No model binaries or weights are committed.

Executed checks cover final configuration/path resolution, calibration/scan/INT8
suppression, real prepared engine batches 1/2/8/1, strict checksum/identity/binding/
profile rejection, GPU fractional/out-of-bounds/nonuniform ROI sampling, pitched
8-bit and packed 10-bit inputs, and the actual GStreamer plugin. Both platforms
pass real pose inference, cadence, continuous-segment retention, discontinuity,
concurrent FLUSH_START cancellation and injected D2H error fencing. Preparation
has eight passing failure/restoration tests, including preservation of the
independent PyTorch attention oracle.

A retained real hockey-player fixture matches MMPose/OpenCV preprocessing exactly
on both GPUs. RTX 5090 FP16 SimCC maximum errors are 0.008336/0.006984; Orin errors
are 0.019063/0.014264. The original fixture-only raw-distribution limit of 0.01
failed on Orin. Diagnosis found exact preprocessing and decoder reduction, with
only near-tied maxima shifting: 15/17 exact joint argmax pairs on RTX and 13/17 on
Orin. Maximum decoded displacement is respectively 0.5 and 0.707 model pixels;
confidence error is 0.001942/0.002454. The fixture now applies the preparation
precision's existing elementwise numerical tolerance while retaining independent
one-model-pixel and 0.01-confidence semantic limits. This is conversion parity for
one fixture, not a hockey pose accuracy assessment.

PARSeq additionally exposed a real TensorRT 10.3 optimization error: the accepted
SDPA-derived ONNX graph produced wrong results even in FP32 with TF32 disabled.
An equivalent, scoped explicit-attention export fixes it without changing weights,
reference behavior or preparation tolerances. The original autoregressive decoder
also requires explicit singleton indexing to avoid unsupported rank-changing If
branches. See [the compatibility findings](parseq-tensorrt-compatibility.md).
Target-local FP16 and FP32 preparation passes batches 1/2/8 on RTX and Orin,
including a real jersey “27” input; every token argmax agrees. FP16 maximum logits
errors are 0.039910/0.035984 and FP32 errors are below 0.000016. The real Orin
fixture reads “27” with confidence 0.833069 versus upstream 0.834680. These results
establish conversion correctness, not jersey recognition accuracy.

A private real-recording x86 pose run completes 317 frames, 357 pose enqueues and
1,487 pose results with no invalid-time/ROI/capacity/cancellation events. Its short
63.92 FPS observation is not a controlled performance comparison. Final disabled,
inference, drawing and ReID performance comparisons remain part of PR 4.


Actual Jetson recording exposed aligned EGL storage: the logical 7135×2634
stitched surface imports as a 7136×2634 CUDA plane. The adapter now requires
storage to cover the logical image and retains the logical width for ROI math.
It does not copy or repack the frame. A native allocation regression covers odd
sizes, existing/caller-owned EGL mappings, stitcher-adjusted plane metadata,
bottom-right pixels, re-import, undersized allocation rejection and unsupported
CUDA-array rejection. The original equality check fails these odd-width fixtures;
the corrected import passes all eight cases. Evidence is in
`/tmp/hstream-player-pr2-egl-test.log` on Jetson.


A completed Jetson pose replay processed 312 frames, 323 pose enqueues and 1,295
pose results, with zero invalid-time/ROI/capacity/cancellation events. One run
then stalled during timed shutdown, after analytics teardown. A native stack
localized the wait to the preexisting lossless mux context mutex. Its CAPS
handler returned from three downstream-event rejection paths without unlocking.
A standalone, GPU-free test reproduces the video and both audio rejection paths
using a downstream flush; the original code hangs and the corrected code permits
NULL transition. The vendor-source patch now releases that lock on those three
failure paths. Buffering, sequence barriers and normal processing are unchanged.
The patch applies to both DS7.1 and DS9.1; full x86/Jetson builds and all rejection
cases pass. Evidence: `/tmp/hstream-player-pr2-shutdown-{build,test,recording}.log`
on each host. Unmodified frozen-master/off/pose repetitions also completed on
Jetson, consistent with an intermittent teardown race rather than inference
failure; the deterministic rejection test is the regression oracle.

The corrected real pose replays complete with “App run successful” on both hosts:
x86 316 frames/356 enqueues/1,483 results; Jetson 312/323/1,295. The standalone
rejection test was also run against the original and corrected vendor objects on
both platforms: all six original cases reach their 10-second failure alarm and
all six corrected cases stop normally. Trimming trailing blank context from the
patch preserves generated source byte-for-byte; final full builds are recorded
in `/tmp/hstream-player-pr2-shutdown-build-final.log`.


PR2 implementation round 1 reviewed `47b2df5a`. The pipeline reviewer found no
necessary fixes. The runtime/preparation reviewer found that export validation
accepted up to 256 supplied examples but tested only the first maximum batch.
The correction retains profile-boundary fixtures, then validates every remaining
row in bounded chunks, records coverage and replays every case during native
preparation. Seventeen CPU tests pass on x86 and Jetson, including all supported
input-count/batch-limit combinations, an incomplete final batch and a failure
occurring only in a later case that must prevent publication. Numerical thresholds
and model graphs are unchanged. Complete x86/Jetson builds pass after the change.
Evidence: `/tmp/hstream-player-pr2-coverage-{build,test}.log` on each host.

PR2 implementation round 2: two independent xhigh reviewers reviewed the
preparation coverage correction against `47b2df5a`; both report no necessary
fixes and independently reran all 17 CPU tests without skips. Prior runtime,
pipeline, EGL and shutdown findings are resolved. The supported engines and
numerical thresholds are unchanged by this preparation-only correction.
## Jersey/action integration (PR 3)

Both complete x86 and native Jetson builds pass after integrating the native
semantic GPU helpers and shared work planner. Integrated x86 tests pass for
configuration/dependencies, weighted/fair aggregate scheduling, 16 exact PIL crop
fixtures (including packed 10-bit and extreme/padded geometry), full-vocabulary
OCR/action reduction, and actual engine batches 1/2/8/1. The retained real jersey
fixture reads “27”; preprocessing is exact. Plugin lifecycle/flush/error tests
continue to pass with all semantic features present.

The actual-model integration test verifies jersey consensus/expiry and identity
reuse, explicit guided crops with no stale-pose/bbox fallback, and two player
histories of 100 samples over 9.9 seconds. A real batched action enqueue reports
label 28, confidence 0.966745 on RTX 5090. This repeated-image fixture verifies
causal processing and model execution, not temporal classification accuracy.
Under a one-sample cap, 105 actual poses split 53/52 between tracks and action
remains unready; a high action cadence cannot reuse a window as new evidence.

A 15-second private x86 recording completes 915 frames, 4,281 pose results,
918 jersey results and 21 action results (10 action enqueues), with no invalid
PTS/ROI, excluded-capacity or cancelled-batch events. Maximum per-frame model
samples is exactly the configured cap of 32; 16 eligible samples were deferred.
History resets/unready counters reflect actual tracking/pose gaps. “App run
successful” confirms shutdown completed. This is operational evidence, not a
controlled performance or hockey accuracy measurement.

Evidence: `/tmp/hstream-player-pr3-{build-initial,config,planner,gpu,engines,jersey,
lifecycle,semantics,recording}.log`. Model weights, fixtures and private game
artifacts are retained outside Git. GPU rendering and final performance comparisons
remain the next stack stage.

Integrated native Jetson tests also pass all seven suites. Its 16 PIL fixtures
have zero pixel differences; the real jersey is “27” at confidence 0.833069.
The two-history action fixture returns label 28 at confidence 0.969337, with the
same 53/52 capacity fairness and gap/reset assertions. A real 20-second Jetson
recording completes 1,212 frames, 4,724 poses, 1,012 jerseys and 26 actions from
22 action enqueues. The maximum remains 32 model samples/frame, with 14 deferred
samples; invalid timestamps/ROIs, capacity exclusions, duplicates and cancelled
batches are zero. It exits naturally with “App run successful”. Remote evidence
is `/tmp/hstream-player-pr3-*.log`, copied locally under
`/tmp/hstream-player-pr3-jetson-evidence/`.


PR3 implementation round 1: two independent xhigh reviewers reviewed
`b071711e..b4a9390b`; neither found necessary fixes. The processor reviewer also
ran fixed-snapshot planner and temporal tests under ASAN/UBSAN. The nonblocking
jersey documentation clarification distinguishes retained brief absences from
expiry, eviction and source/seek resets.


## GPU drawing and controls (PR 4)

The integrated x86 build passes, including the explicit CUDA/GL compositor and
preview targets. Focused metadata/command, actual cropper, preview collector,
routing, canonical mapping, desktop controls/window and actual semantic-engine
tests pass. Default/empty rendering is checked before allocation; disabled model
sentinels are not accessed. Independent tests exercise real DeepStream metadata
copies, a shared color producer, full track IDs/overflow, rotated/nonuniform
coordinates, current poses, expiry, immutable tee inputs and baked-layer suppression.

The actual GPU cropper test checks pixels at an independently derived rotated
position, leaves its input byte-identical, and verifies stream completion before
fixture readback. Capacity suppression preserves output video. Shutdown releases
resources once and is idempotent. The Jetson completion fence now outlives queued
work but is destroyed before per-frame EGL imports; this closes an existing
import/fence ordering gap. The actual preview collector suppresses missing-transform
frames and clears previously built commands, while Stitched retains independent
unbaked drawing.

Integration caught and corrected zero-confidence joints at a zero display threshold,
semantic baked-box masking, optional canonical-null CLI/UI consistency, private STB
font symbol collisions, and stop-owned renderer cleanup. Fully offscreen successful
commands are no-op renders, not capacity/error suppression. These fixes have
regression coverage or real pipeline counter evidence.

Real x86 runs complete through FAKE and HEVC/AAC file output, with GPU pose/box
drawing and no suppressed/rejected commands. The encoded fixture is 7680×4320,
5.272 seconds, and probes successfully. All-model Program/Stitched GPU previews
also complete with and without vpplaytracker; native-tracker fallback places the
preview tee and shared color owner after semantic inference. Preview uses the
existing framebuffer, with no new full-frame copy. Source/encode fixtures stay
outside Git.

Local integrated logs: `/tmp/hstream-player-pr4-{build-integrated,overlay,
overlay-gl,overlay-contract,cropper-caller,preview-caller,routing,mapping,
semantic-labels,ui-controls,ui-window}.log`; real recording/graph logs are under
`/tmp/hstream-player-analytics-validation/pr4-*`. Final platform/performance and
paired PR review results are recorded below.

The complete native Jetson build passes. Its six supported focused suites cover
the CUDA compositor, overlay contracts, actual GPU cropper, routing, mapping and
color configuration. Desktop Qt/GL tests run on x86; those targets are intentionally
incompatible with the repository's Jetson platform. No native ARM64/SBSA host was
available, so Jetson validation is not evidence for an SBSA runtime.

An initial, pre-review native Jetson 20-second all-model drawing replay completes 1,212 frames,
4,724 poses, 1,012 jersey results and 26 action results from 22 action enqueues.
Drawing reports 1,190 renders, 1,149 launches and zero suppressed/rejected commands.
The retained compositor allocation is 6,438,912 device bytes. Invalid timestamps
and ROIs remain zero, and the process exits successfully.

### Controlled x86 performance

Thirty final-source unprofiled runs use frozen master/candidate runtime bundles, sampled loaded
plugin paths and hashes, the identical explicitly pinned FP32 detector engine,
private calibration/media, FAKE output, 15 seconds of media per run, and three
rounds with reversed case order. The GPU is an RTX 5090 with driver 610.57.04;
power settings and clocks were not changed. Source/build-file hashes bind the
candidate to clean runtime commit `e3ed7898`; later `9b597543` changes only
benchmark parsing/docs. Artifact guards pass throughout. This complete renewed
suite supersedes the initial PR4 timing/trace evidence retained separately.

Each run drops its first periodic FPS sample. The table reports the median of
three run medians and the maximum sampled process-tree NVML memory. Enabled
models use pose/jersey/action cadences of 10/2/1 Hz, batch limit 8 and aggregate
limit 32 model samples/frame. Jersey-only uses bbox crops; action requires pose.

| Case | Median FPS | Peak GPU MiB |
| --- | ---: | ---: |
| Frozen master | 62.925 | 10560 |
| Candidate, all off | 63.170 | 10560 |
| Pose | 62.015 | 10586 |
| Jersey only | 62.645 | 10686 |
| Pose + action | 61.960 | 10728 |
| All three | 61.715 | 10804 |
| All three + semantic drawing | 61.470 | 10810 |
| Colored boxes only | 63.250 | 10566 |
| Native ReID only | 62.010 | 10844 |
| All three + drawing + ReID | 60.840 | 11092 |

Median *paired* FPS differences are +0.008% for disabled versus master, -2.557%
for all three versus disabled, and -0.397% for drawing versus all three. These
use each round's reference and differ from ratios of overall medians. Disabled
versus master spans -0.429% to +0.389%; this sample shows no meaningful disabled
regression or new measured GPU allocation. Drawing comparisons span -0.534% to
-0.292%. They describe this recording, not a universal overhead guarantee.

Every enabled run records actual inference, including 21 action results in each
all-model run. Drawing runs report roughly 900 raster launches and zero rejected
or suppressed commands. Display labels remain confidence-gated, so this is not
a maximum-label workload or an accuracy assessment. Timed source stops can differ
by a few completed frames; per-run samples and counters are retained. FPS measures
throughput, not individual frame latency; startup/shutdown wall times are recorded
separately. Trace runs are excluded from the throughput sample.

Local evidence: `/tmp/hstream-player-pr4-performance-e3ed7898/summary.json`,
`results-x86-measured/`, `verified-plugin-identity.json`, and frozen runtime/model/
calibration manifests. Private media, models and machine-specific specifications
are not committed.

A separate Nsight Systems capture passes transfer attribution: 1,037 pose, 422
jersey and 10 action D2H copies match both GPU reducers and teardown enqueue
counters exactly. The largest result transfer is 1,632 bytes (eight poses), with
881,260 bytes total. The Program drawing stream executes 893 raster kernels,
matching its launch counter, and contains zero D2H transfers after drawing starts.
The retained global transfer histogram separates existing detector/tracker copies
from these analytics streams. This execution contains no analytics video/crop
readback; offline parity tests intentionally read larger tensors/images. Raw
`.nsys-rep`, SQLite and `transfers.json` remain in `profile-all3-draw/` beside the
benchmark evidence. No native Jetson Nsight capture was available.

The corrected parser independently recomputed all medians from raw logs,
retaining any zero-FPS observations after warmup. An audit of all 62 old/new
measured and initialization logs found no zero observations, so these corrections
do not change the measured x86 results. Original reports remain intact beside
`results-offline-verified.json` and `zero-fps-audit.json`.

### Native Jetson performance

The final enabled matrix uses a frozen runtime from `9b597543`, native
DeepStream 7.1/TensorRT 10.3, the same pinned FP32 detector and target-local
analytics engines, and a private 7135×2634 canvas from two 4K camera sources.
Each case processes 20 seconds of media to a FAKE sink, with the same
10/2/1 Hz pose/jersey/action rates, batch eight and aggregate cap 32 as x86.
High-bit-depth output is disabled for this fixture; MAXN/schedutil and clocks
remain unchanged. Two rounds reverse the order of eight enabled cases.

All 16 final runs complete successfully. The table retains each run median to
show variation; each run excludes the startup prefix through its first two
positive FPS observations, then retains every interval, including any zero.

| Case | Forward FPS | Reverse FPS | Median FPS |
| --- | ---: | ---: | ---: |
| Pose | 7.290 | 7.395 | 7.343 |
| Jersey only | 7.550 | 8.010 | 7.780 |
| Pose + action | 7.360 | 7.390 | 7.375 |
| All three | 7.170 | 7.305 | 7.238 |
| All three + semantic drawing | 7.110 | 7.280 | 7.195 |
| Colored boxes only | 8.080 | 8.495 | 8.288 |
| Native ReID only | 8.190 | 8.140 | 8.165 |
| All three + drawing + ReID | 7.160 | 7.140 | 7.150 |

The paired drawing changes are -0.837% and -0.342%, median -0.590%. Both drawing
runs produce actual inference and raster work with zero suppressed/rejected
commands: forward 1,185 renders/1,149 launches, reverse 1,184/1,148. Their
retained compositor allocation is 6,438,912 device bytes. All-model runs produce
26 action results after the required history warmup.
The enabled-model table is not paired against a new final-source disabled run,
so it does not establish precise model-only percentage costs. These are
throughput observations for this fixture, not frame latency or an accuracy test.

Disabled-path evidence is retained separately. Two valid alternating pairs use
the original PR4 runtime corresponding to `88eec689` and frozen master:
master/off 8.09/8.435 FPS, then off/master 8.36/8.19 FPS. Their overall medians
are 8.140/8.3975 FPS and the median paired change is +3.170%. The original runtime
was frozen before its commit; comparison against `88eec689` confirms all compiled
source/build/config files match (only two unrelated editor-preference files
vary). A separate final `9b597543` disabled confirmation completes at 8.24 FPS
with no analytics/drawing and a peak process RSS of 5291.23 MiB. Do not pool
that confirmation with the older paired sample.

Two additional frozen-master attempts (the original timeout and one bounded
retry) reach clip completion but time out during shutdown at 360 seconds (exit
124). They are excluded and preserved; no further retries were attempted. No
candidate matrix run has this failure. Native ptrace policy prevents a GDB
attach, so retained process wait observations do not establish the cause. The
successful pairs and final confirmation support compatibility but are a small
sample, not a tight universal disabled-path performance bound.

Jetson shares system DRAM. Sampled process RSS is not GPU memory, and tegrastats
RAM describes the whole system; neither is reported as per-process GPU usage.
No native Nsight capture was available. The x86 trace and focused native tests
provide the separate transfer/allocation evidence above.

The private harness initially rejected a successful forward ReID run because
DS7.1 uses generic engine-load messages instead of DS9.1's path-bearing wording.
Versioned evidence rules revalidate the original logs without changing or
rerunning production inputs. Later runs retain the generated tracker YAML and
verify engine binding, `reidType: 2`, batch eight and disabled embedding export.
The first forward ReID-only temporary YAML had already been deleted normally;
later same-input YAML is not represented as a capture from that earlier run.

Native logs, runtime/source/model identities, versioned harness specifications,
memory samples and summaries remain under `/tmp/hstream-player-pr4-performance/`
on `stubby` (backed by its NVMe directory). Compact evidence is copied locally to
`/tmp/hstream-player-pr4-jetson-evidence/`, including the original-source commit
comparison and separate disabled summary.

### Isolated renderer cost

The synthetic fixture draws 32 players with boxes, 19 bones, 17 joints and text:
1,472 commands after at least 500 ms warmup, then 200 samples. RGBA8 wall medians
(command construction, binning, submission and completion) are 91.9/154.3 µs at
1080p/4K on RTX 5090 and 753.2/1576.3 µs on Orin. GPU upload+raster medians are
25.6/33.1 µs and 511.1/1119.7 µs respectively. Packed RGB10A2 tests also pass;
the actual Program cropper continues to use its existing RGBA path.

Every warmed synthetic frame uses one compact metadata upload and one raster
launch, with zero C++ heap/device/pinned allocations and no atlas re-upload.
Empty drawing makes no CUDA calls or allocations. The shared glyph atlas is
147,456 bytes; bounded three-slot device and pinned buffers are each at most
6,438,912 bytes including the atlas. The x86 GL fixture measures approximately
52/59 µs wall and 4.8/11.4 µs GPU at 1080p/4K. These isolated 4K timings do not
establish the cost of the real 8K Program output. Jetson clocks were not fixed;
its RGBA8 wall p95 is 1169.7/2241.8 µs. The Orin values use the final
`9b597543` frozen test/dependency bundle. An initial missing-library launch
failed before any GPU work; the completed dependency closure passes the fixture.
x86 CUDA memcheck reports zero errors.

### PR4 review iterations

Two independent xhigh reviewers reviewed `88eec689`. Both reported one required
correction: unchanged desktop controls could override boxes inherited from
`plot.debug_play_tracker`, and labels for fully cropped-out players could be
clamped onto the Program edge. The controls now resolve the legacy boolean OR
with its source ranks and omit unchanged box overrides; explicit edits preserve
other debug layers. Labels require actual transformed-box/viewport intersection
before anchor clamping, including rotated boxes whose bounding rectangles alone
overlap. Regression cases cover saved/exported/active UI arguments, all four crop
sides, partial visibility and rotated geometry. Complete x86 build and focused
controls/window, overlay-contract and actual GPU cropper/preview checks pass.
Native rebuild, renewed performance evidence and paired second review are recorded
below and in the performance sections above.

Follow-up inspection caught a benchmark-only parser error that removed all zero-FPS
observations before warmup selection. The parser now retains raw zeros and includes
them after the explicit warmup prefix. A real subprocess regression with
`[60, 0, 0, 30]` verifies the post-warmup median is zero, not 30. Existing evidence
is audited/reparsed from retained logs; this correction does not change runtime
code or require repeating GPU workloads.


PR4 formal round 2: two independent xhigh reviewers reviewed published head
`9b597543` and found no necessary fixes. The renderer reviewer rebuilt the
original isolated offcrop CUDA reproducer against the exact head: zero emitted
commands and zero changed pixels, versus nine commands and 232 pixels before
the fix. Eight overlay test groups and all seven benchmark/profile tests pass.
The integration reviewer independently checked Configurator's OR/source ranks,
unchanged and explicit box preferences, save/export/active-run snapshots and
script cleanup/zero-FPS semantics; controls/window/overlay and seven isolated
Python tests pass. The optional suggestion to broaden `--validate-only` case
preflight is nonblocking.

Complete final-source x86 and native Jetson builds pass. Native affected overlay
and actual GPU cropper regressions pass, as do all seven CPU harness tests. The
final native runtime has the same production source as `e3ed7898`; `9b597543`
adds only the benchmark correction/documentation. Logs are
`/tmp/hstream-player-pr4-review-final-x86-build.log` locally and
`/tmp/hstream-player-pr4-review-fixes-build.log` on Jetson.

Final handoff rebuilds also pass (341 x86 targets including manual GPU tests;
339 native Jetson targets). The remaining handoff edits are documentation only;
production source is unchanged. Logs are retained locally as
`/tmp/hstream-player-pr4-handoff-{x86,jetson}-build.log`.

### Normal-build SDK selection follow-up

The initial validation supplied a private `HSTREAM_PLAYER_TENSORRT_SDK_ROOT`,
which concealed a normal-build failure when DeepStream 9.1's TensorRT 10.16.1
runtime coexists with TensorRT 11 development packages. Removing the
`tensorrt-dev` metapackage does not remove those component headers/libraries.
The repository now selects DeepStream's versioned libraries automatically,
checks the complete native header/runtime version, and uses pinned NVIDIA
10.16.1.11 development headers inside Bazel when installed headers disagree.
Normal build/playback needs no SDK override and no system package changes.
Explicit custom overrides remain strict; sysroot builds never execute target
libraries or select the managed x86 headers. See the
[preparation guide](player-model-preparation.md) for exact selection boundaries.

The exact `make perf` command passes with the override unset in the ordinary
output base. The first full build executed 3,538 actions; the final rule also
passes the full build after version/multiarch checks. Fifteen isolated regression
cases use real ELF libraries and deb extraction to check mixed-major/minor/build
versions, missing headers, strict overrides, sysroot isolation, parser identity,
multiarch selection and the DeepStream root override. The player builder reports
TensorRT 10.16.1 build 11, and actual pose-engine validation passes batches 1/2/8/1
plus malformed-contract rejection. A real five-second recording completes with
`App run successful` and no analytics element under default settings.

Native Jetson's full 338-target build passes with the override unset. It retains
its installed AArch64 headers and versioned libraries, creates no managed header
downloads, and reports TensorRT 10.3.0 build 30 / CUDA 12.6 on Orin. Neither native
playback nor model preparation imports the TensorRT Python package.

Both follow-up reviewers found one additional SDK consistency gap: installed
headers and explicit SDKs could bypass the ONNX parser release check. All
selection paths now compare the parser and inference release; two additional
regressions cover those cases.

Evidence: `/tmp/hstream-player-sdk-{make-perf,make-perf-final,selection-tests,
runtime-info,engine-test,default-playback}.log` and
`/tmp/hstream-player-trt-sdk-jetson-evidence/validation.md`.
