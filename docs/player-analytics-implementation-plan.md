# Player analytics implementation plan

Status: approved after two rounds by both independent xhigh reviewers. Implementation in progress. Implements the reviewed
[design](player-analytics-design.md) and [model comparison](player-analytics-models.md).

## Delivery order

Four ready-for-review PRs form a linear stack. PR 1 targets master; each subsequent
PR targets its predecessor. Do not merge the stack during this task. Commit complete,
buildable stages, record exact base/head revisions and validation, and run two reviewers
after opening each PR. Fix required findings, rebuild/retest affected paths on both
machines, and run another pair until only nonblocking suggestions remain. Any review or
hardware limitation must be reported rather than silently counted as a passing check.

### PR 1: bounded metadata/state, track colors, optional native ReID

1. Add `src/libs/player_analytics/` with small dependency-separated targets for config,
   model contracts, media-time scheduling, track identity/state, and 32-color leases.
   Keep pure CPU tests independent of TensorRT. Define immutable result metadata with
   noexcept DeepStream copy/release and finite hard caps; no model/runtime initialization
   in static constructors. A versioned manifest binds fixed model family/preprocess,
   names/shapes/types, label/token dictionary, engine hash, GPU and TRT identity.
2. Extend existing requested preview snapshot ownership with a single color allocator.
   Preserve full IDs and visible color leases, reclaim inactive leases before collisions,
   handle >32-visible sharing, untracked IDs, epochs, geometry changes, and bounded expiry.
   Sharing is temporary: preserve the established owner and move overflow leases to
   newly available colors so uniqueness returns when visible count is at most 32.
   Copy allocated colors into immutable snapshots and use them in existing CUDA box
   drawing and GL previews. Establish requested post-tracker fallback ownership without
   vpplaytracker, including the actual Stitched tee routing in this PR (not PR4).
   Producer demand is the union of Program output drawing (including an encoded-only
   run without any preview window) and preview layer requests. Preserve the existing preview-only
   Player boxes toggle: it does not burn anything into encoded output. Never attach a
   new probe to ordinary all-off playback.
3. Add `pipeline.tracker.reid-enable` and `reid-config-file` parsing. With ReID disabled,
   skip all new file/cache work and leave native tracker settings untouched. With it
   enabled, validate and merge a small overlay with the selected NvDCF config into an
   owned runtime file. Require the configured prebuilt engine, resolve paths correctly,
   remove ONNX/ETLT rebuild inputs, preserve unrelated tracker tuning, bound gallery and
   extraction settings, and validate DS7.1/DS9 compatibility. Reject incompatible tracker
   choices and sub-batch configs unless fully supported. Perform this preparation at
   final graph construction, after calibration/timed-mask/player-scan/INT8 mode selection
   and only if native tracking is enabled. Clean-only and disabled modes do no ReID I/O.
   Add an explicit offline preparation recipe using a pinned obtainable reference ReID
   model: NVIDIA ReIdentificationNet deployable_v1.2
   `resnet50_market1501_aicity156.onnx`, SHA256
   `0e21d09278508ec835955f422a9fdd3cd59b2a6ecdef98d705f388f33cebac2b`, from the
   [official NGC download](https://api.ngc.nvidia.com/v2/models/org/nvidia/team/tao/reidentificationnet/deployable_v1.2/files?redirect=true&path=resnet50_market1501_aicity156.onnx).
   Bind RGB/NCHW `[B,3,256,128]`, 256 output features, offsets
   `[123.675,116.28,103.53]`, scale `0.01735207357279195`, stretched resize (`keepAspc=0`) and L2 output
   normalization, matching this ONNX model's official NVIDIA preprocessing (the old
   ETLT template's keepAspc=1 is not its contract).
   Record matching preprocessing/TensorRT SDK, checksum/provenance and per-GPU engine
   output. A real model running in nvtracker on both hosts is a delivery gate; config-only
   tests or unrelated fixture networks do not establish appearance reassociation support.
   Own/delete only generated files
   after tracker teardown. Add example configuration and explicit preparation directions.
4. Add design/model docs, configuration/lifecycle tests, and update AGENTS.md with actual
   new responsibilities. Do not advertise pose/jersey/action switches as operational yet.

Validation: CPU state/config tests; metadata copy ownership and allocation failure;
color sequences including overlapping old/new visible sets, temporary overflow leases
   and 33→32 recovery when a nonshared color owner leaves; off-path inaccessible model
paths ignored; ReID merged config and no-rebuild tests; x86 all-target build, native
Jetson all-target build, GPU previews with color consistency. Verify native ReID engine
loading with the reference prepared compatible model on both hardware platforms.
A model/export incompatibility must be resolved before declaring ReID operational.

### PR 2: GPU TensorRT runtime and working pose inference

1. Add a narrow TensorRT runtime target using the actual DeepStream-compatible SDK,
   separately from the overrideable detector builder. Check the DeepStream-linked
   `libnvinfer` major against selected headers/libraries at configure/startup; fail early
   on mismatch. Support an explicit runtime SDK root without changing the system install.
   This machine needs the extracted TRT10 SDK because default development headers and
   Python are TRT11 while DeepStream links TRT10.16. Jetson uses native TRT10.3.
   Use the same coherent SDK for the new offline builder. Implement strict named tensor
   validation, bounded dynamic batch, checked allocation arithmetic, RAII context/buffer
   ownership, one owned stream, GPU result reduction, compact result copies, and error
   fencing. No CPU fallback, asynchronous detached worker, device-wide synchronization,
   runtime ONNX parsing or engine building.
2. Add `gst-player-analytics` in-place metadata plugin, its runtime-plugin data/package
   entries, and a small app bin/config adapter. Resolve enabled config after layering;
   graph construction creates the element only when a compute feature is enabled.
   Place it after nvtracker before vpplaytracker. Explicitly suppress it for calibration,
   mask, frame-selection scans and INT8 sampling before any asset/model discovery.
3. Implement GPU ROI affine sampling from existing NVMM/EGL surfaces, exact RGB/mean/std
   preprocessing, RTMPose SimCC argmax/confidence reduction, inverse affine mapping, and
   same-frame immutable pose metadata. Consume only tracked person detections, use
   source-time scheduling, cap due work fairly, and reset/fence on discontinuity/flush.
   Validate metadata-to-surface scaling and supported caps at startup. No stale skeleton
   spatial carry on skipped frames.
4. Add an offline preparation command with RTMPose export from a real checkpoint, a
   native engine builder matching runtime SDK, atomic model bundles, provenance, and
   PyTorch/ONNX/TRT parity checks. Use the actual downloaded M COCO17 checkpoint/config;
   record SHA256 and flip-test-off deployment recipe. Do not modify shared hm baseline.
   HARPET18 support is optional only after its explicit topology contract is verified;
   it is not a prerequisite for the supported full COCO17 pipeline.

Validation: real model parse/build/inference; nontrivial reference tensor and crop parity;
GPU preprocess/decoder tests with fractional/padded/asymmetric ROIs; malformed binding
rejection; off-path topology and file-access proof; graph error/stop/flush tests; x86 and
Jetson builds; real-recording pose metadata and bounded memory. Pose drawing lands PR4.

### PR 3: working jersey and action inference, temporal correctness

1. Add strict PARSeq checkpoint export with actual tokenizer/character mapping, explicit
   fixed max-length=2 and EOS semantics, AR/refinement recipe, bicubic RGB normalization,
   and local rights/provenance documentation. Preprocess parity includes the reference
   bicubic antialias/border rules, with high-frequency downsampling fixtures; a four-tap
   sampler alone is not assumed equivalent to PIL/torchvision. Export generic reference weights when a
   supported hockey checkpoint is unavailable; state domain accuracy limitations.
2. Implement bbox-torso and explicit pose-guided ROI modes, GPU crop sampling and compact
   digit/EOS decoding, minimum-size/visibility/confidence filtering, leading-zero strings,
   and one bounded source-time-decayed vote/hysteresis accumulator. No second tracker,
   implicit pose load, identity merges from jersey numbers, or indefinite stale labels.
3. Export actual STGCN++ COCO2D backbone/head through a dedicated wrapper. Enforce
   `[B,2,100,17,3]`, zero second person, full-canvas normalization, COCO17 layout and
   matching 60-class NTU label map. Build uniformly sampled causal windows from fresh
   pose observations, reject insufficient/gapped history, run bounded low-rate inference,
   and publish confidence/window/expiry with label hysteresis. Document single-view
   causal semantics separately from offline ten-clip accuracy. Never invent hockey labels.
4. Extend preparation parity tests and native runtime tests across all real models.
   Add diagnostic counters for work skipped by cadence/capacity/visibility and evidence
   readiness; logs are bounded and no video readback is added for diagnostics.

Validation: real PyTorch→ONNX→TRT parity for both models, exact PARSeq token fixtures,
jersey zero/leading-zero/unknown/conflicting-vote tests, action gaps/duplicates/short
history/seek tests, jitter/capacity skips/invalid PTS and continuous chapter tests,
combinations and dependency errors, x86 and Jetson builds, actual
recording inference long enough to pass the 9.9 s warmup, asserting at least one actual
action enqueue/result with the expected window. Report insufficient history explicitly.
Record model identity and separate throughput/VRAM results.

### PR 4: GPU overlays, controls, integration and performance evidence

1. Add a narrow batched analytics compositor in `draw_display`: bounded lines/discs,
   rectangle strips, cached glyph atlas, sparse tile command bins (hard caps also on active
   tiles, command-to-tile references, glyphs, and upload bytes), deterministic pixel
   ownership, and reusable GPU buffers. Draw directly onto owned Program output after
   crop with its existing stream/fence. Empty/disabled layers cause no metadata scan,
   commands, allocations, uploads or launches. Test pitched 8-bit and packed 10-bit
   kernel formats; do not claim unsupported upstream 10-bit cropper negotiation works.
2. Add matching GPU preview geometry/text consuming the immutable analytics snapshot,
   exact existing Program transform, and allocated colors. Attach transform for analytics
   independently of old debug overlays, select the correct pre-crop tracked tee without
   vpplaytracker, and carry baked-layer bits to avoid duplicate Program drawing. Preserve
   original play-debug layers and raw detection telemetry behavior.
3. Map existing canonical plot flags with ordinary explicit-layer precedence. Add next-run
   desktop controls for compute/model bundles/ReID and independent drawing preferences;
   persist via existing game/user config helpers, preserve unknown keys, show missing
   prerequisites before launch, and include settings in exported jobs. Do not infer compute
   enable from drawing. Respect current generation/acknowledgment ownership; no hot model
   replacement is introduced. Ship commented config examples and preparation/run commands.
4. Preserve unchanged-master runtime bundles (executable, own plugins, dependent runtime
   libraries and configs with content hashes) on x86 and Jetson before source changes.
   Record representative warmed baselines on both. Use identical private game artifacts,
   start/duration/sinks and power modes for candidate comparisons. Add a repeatable
   benchmark/inspection command that alternates frozen-master/candidate-off and then
   inference/drawing comparisons and
   records model/SDK/GPU/input/output identity, frames/FPS/latency and process GPU memory.
   A GPU synthetic metadata test isolates render cost from inference and reports CUDA
   timings, allocations, and transfer sizes. Run actual preview/FAKE/encode paths and
   a profiler trace to confirm no video/crop readback. Fix measured disabled/rendering
   regressions before making performance claims; record unresolved limitations explicitly.

Validation: meaningful GPU pixel/overlap/pitch/transform tests; deterministic same colors
across views; no duplicate overlays; all-disabled topology/resource comparison; actual
recording seek/restart/chapter behavior; complete x86 and Jetson builds; default versus
each model, colors/drawing, ReID and combined benchmarks. Update architecture/user docs
to actual supported behavior and summarize all review rounds in the PR handoff.

## Implementation boundaries and shared decisions

* Root owns stack/commits/PRs, configuration/pipeline integration, and cross-platform
  validation. Delegate disjoint libraries, model preparation, ReID, and drawing work
  only after interface ownership is agreed; contributors do not commit shared files.
* Native inference configuration remains absent/off by default. New structural examples
  do not cause model resolution. Preexisting canonical drawing flags keep their meaning
  as preferences. The pipeline's existing operational source/track/generation remains
  authoritative. Analytics is diagnostic and does not change camera policy/detections.
* Model artifacts are outside Git. Native builder and runtime report matching TRT/GPU
  identity. The installed Python TensorRT package is a different major version and must
  not build runtime engines. Conversion dependencies are isolated in an offline env.
* Use bounded synthetic semantic fixtures for lifecycle and drawing tests; use real
  pretrained artifacts for model parity and runtime claims. Neither substitutes for the
  other. Never report fixture-model inference as validation of the deployed checkpoint.
* Both hosts share the source worktree through NFS. Avoid concurrent edits/build symlinks
  from remote Bazel: use a distinct `--symlink_prefix` on stubby. Keep validation logs and
  fixture outputs outside the repository, and never mutate original game calibration.
