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
camera media, FAKE sink, 15 seconds of video, and the same detector engine:

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
