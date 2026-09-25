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
