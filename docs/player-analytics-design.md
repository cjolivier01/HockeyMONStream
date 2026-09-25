# Optional player analytics

Status: reviewed design; implementation status is tracked in the implementation plan.
The three xhigh design reviewers completed two rounds; decisions below incorporate their findings.

## Requirements and performance contract

Pose estimation, jersey-number recognition, and action recognition are independent,
explicitly enabled features. Ordinary playback must not load their models, construct
their inference element, allocate their GPU buffers, download their assets, or invoke
an analytics probe. Merely selecting a drawing preference must not enable inference.
Enabling action recognition requires a compatible pose model and reports the dependency
at startup. Inference controls apply on the next run; there is no hot model replacement.

Enabled inference has an unavoidable compute cost. Bounded object counts, batches,
cadences, and history lengths make that cost controllable. Rendering must use existing
GPU surfaces and GPU preview interop: no frame/crop readback, full-frame overlay staging
copy, CPU image conversion, or kernel launch for every individual joint. Compact model
results and drawing commands may cross the CPU/GPU boundary, with explicit size bounds.

Measure separately: unchanged-master versus all features disabled; inference without
drawing; drawing with fixed synthetic results; each enabled model; ReID alone; all
enabled features. Record completed frames, throughput, latency, peak/steady GPU memory,
and model/cadence/batch/precision identities. Repeat warmed runs and report variability;
do not claim a meaningful speedup from external benchmark numbers. Initial acceptance
targets are no extra disabled-path GPU allocation and throughput within baseline noise
(investigate a repeatable >2% regression), and <1% throughput cost for drawing at normal
player counts. Enabled-model costs are reported rather than hidden in a rendering claim.

## Ownership and graph

```text
hmstitcher -> primary inference -> field mask -> nvtracker
  -> [hmplayeranalytics, only when an inference feature is enabled]
  -> vpplaytracker -> tracked Stitched tee -> playcropper -> Program sinks
                                    \-> GPU Stitched preview
```

Keep the existing secondary-inference bin unchanged. It is downstream of vpplaytracker,
and its tee/refcount synchronization does not provide the ownership needed here.
The optional in-place GStreamer element owns a native TensorRT runtime, one CUDA stream,
bounded reusable input/output buffers, and per-track state. It reads NvBufSurface GPU
memory through the existing CUDA/EGL mapping helper; mapping a buffer descriptor must
never be confused with mapping frame pixels into system memory. It passes every input
frame onward and never inserts leaky queues or changes recording completeness.

Use the TensorRT named-tensor/enqueueV3 API shared by the installed x86 DeepStream and
Jetson DeepStream 7.1/TensorRT 10.3. Runtime libraries must match the DeepStream process.
Do not reuse the current ONNX Session CPU-input/output wrappers for player crops.
Do not load the DeepStream inference-context library directly: versions differ in
output-copy controls and the project already avoids its logger symbol interposition.

Run all selected work for a batch on the owned stream, wait for that stream's result
event before publishing compact immutable metadata, and finish before releasing the
input buffer. No detached jobs retain GstBuffer pointers, NvDsObjectMeta pointers, or
borrowed surfaces across frames. Fatal errors post a GStreamer bus error and stop the
element. Stop/cancellation releases contexts and buffers after outstanding work ends.

## Configuration and prepared models

Native settings live under `pipeline.player-analytics`, with nested pose, jersey, and
action enable flags, model selections, optional custom paths, update rates, confidence thresholds, and global
object/batch limits. All enable flags default false. Existing synchronized canonical
`plot.plot_pose`, `plot.plot_jersey_numbers`, and `plot.plot_actions` map to drawing
preferences using the normal provenance rules; explicit native properties win at the
same layer. Preserve the shared baseline byte for byte. New settings are parsed only
after layer resolution. Enabled built-in selections download and prepare automatically
before pipeline allocation. Invalid custom models fail clearly; disabled features
ignore their paths and require no model tooling.

Each prepared bundle binds the ONNX/engine digest, model family, tensor names and
shapes, maximum batch, normalization, crop convention, joint layout or character/label
vocabulary, GPU architecture, and TensorRT version. Supported portable graphs are
SHA256-pinned, and startup prepares engines in a short-lived native child. Exporting
new/custom graphs remains an offline source-checkout workflow. Playback is native
and Python-free. Publish prepared models
atomically in user cache storage; never write engines into source, media, or package
directories. Preparation must compare PyTorch, ONNX, and TensorRT results on nontrivial
inputs before advertising support. Do not commit checkpoints or engines.

## Selected deployment models

* Pose: RTMPose with exact affine crop/normalization and SimCC decoding. Start with a
  COCO17 model for the full pipeline. HockeyMON's HARPET18 model includes stick endpoints
  and requires an explicit topology; it cannot feed a COCO17 action model by truncation.
* Jersey: PARSeq with a fixed export contract, a configurable bbox torso or pose-guided crop, digit/EOS decoding,
  and temporal consensus. Bbox mode works independently; pose mode requires explicitly enabled pose. Preserve leading zeroes. Reject illegible, low-confidence,
  non-digit, and unsupported-length strings. Generic scene-text weights do not establish
  hockey accuracy. The converted Koshkina model downloads automatically with its
  CC BY-NC 3.0 attribution and noncommercial terms; conversion does not relicense it.
* Action: use the lightweight STGCN++ COCO2D model with a dedicated export/parity
  wrapper. Generic NTU60 labels remain generic activities, not hockey-event predictions.
  A causal single-view deployment must be described separately from published ten-clip
  evaluation. The existing PoseC3D multi-view recipe is too costly as a default deployment.
* ReID: use nvtracker's own appearance reassociation. The default uses the SDK-supplied
  model and preprocessing; the NVIDIA ONNX alternative and custom configurations are
  explicit selections. Do not equate identical jersey numbers with the same player.

The accompanying [model comparison](player-analytics-models.md) records alternatives,
actual artifacts, licenses, export support, and benchmark limitations. The supported
initial contracts are RTMPose-M COCO17 `[B,3,256,192]` to two SimCC tensors
`[B,17,384]`/`[B,17,512]`, with 1.25 affine padding and RGB ImageNet normalization
(single forward, flip-test off); PARSeq fixed two-character decoding with three output
positions, RGB bicubic resizing and `pixel/127.5-1` normalization; and STGCN++
`[B,2,100,17,3]` to 60 NTU logits. STGCN++ uses full-canvas coordinate normalization
and a zero-padded second person. Its 100 causal uniform source-time samples are a
separate deployment recipe from ten-clip evaluation. PARSeq's tokenizer, EOS and
character indices come from the actual checkpoint; never assume the first 11 logits
are digits. Strict weight loading and real ONNX/TRT parity are delivery gates.
Koshkina model terms include noncommercial restrictions. Automatic delivery and local
preparation both retain those terms. Published converted ONNX files include source and
modification attribution plus complete license notices; private validation fixtures and
GPU-specific engines are excluded.

## Coordinates, state, and metadata

All analytics payloads use pre-crop DeepStream metadata coordinates, before Program crop/rotation.
Metadata dimensions can differ from surface dimensions: GPU cropping applies explicit
per-axis metadata-to-surface scaling, and decoding applies its inverse before publication.
Every ROI stores its exact inverse affine transform, including expansion and padding.
Postprocessing maps joints back through that transform. Validate dimensions, bounds,
finite coordinates, confidence, and tensor contracts before dereferencing buffers.

Track identity is `(stream, epoch, full 64-bit object ID)`. Never assign persistent state
to the untracked sentinel. State is bounded by configured maximum tracks and a finite
absence timeout. Reset on stream start, flush/seek, discontinuity, backward timestamps,
and stitched geometry changes; model changes require restart. FLUSH_START increments
an atomic publication epoch; completion checks that epoch before attaching results.
Always retire queued GPU work before releasing a source surface, even on errors.
Continuous chapter SEGMENT events preserve history when PTS/sequence/geometry remain
continuous; pause/resume and Program crop changes preserve pre-crop state. Eviction releases pose history, votes, and labels. Initial limits: 256 retained tracks,
32 due ROIs per frame, inference batches at most 8, and 100 skeleton samples per action
track. Admit new tracks after expiring inactive records; at capacity exclude new IDs
with an observable counter instead of evicting visible identities. Use deterministic
oldest-due scheduling so the object limit does not starve the same players. Retention
uses source time and is distinct from nvtracker's longer reassociation horizon; a
reassociated ID whose analytics expired starts fresh evidence. Returning after eviction creates fresh evidence.
Do not silently merge evidence from two tracker IDs. Reassociation is nvtracker's job.

Schedule by source timestamps, not wall-clock or modulo frame numbers. Never invent
fresh pose measurements on skipped frames or double-vote duplicate observations;
skeleton drawing uses fresh same-frame results only. Action windows use uniform source-time
samples, a compatible layout, enough valid measurements, a bounded gap, and a fixed
causal history. The initial STGCN++ profile uses 100 samples at 100 ms spacing (9.9 s),
from continuous stitched `buf_pts`, never chapter-local decoded source timestamps.
Interpolate only between fresh bracketing observations at most 150 ms apart, using
minimum endpoint confidence; never extrapolate. Invalid time/gaps make the action
unready. At least 10 Hz pose updates and a real run beyond warmup are required. A gap resets the action window. Pose, jersey, and action outputs carry
their observation time and expiry. Use bounded confidence-weighted jersey consensus
and label hysteresis; never hold an unsupported action forever.

Publish versioned immutable frame metadata containing stream/epoch/frame identity,
coordinate dimensions, per-track color, bounded keypoints, jersey string, action label,
confidence, and measurement time. DeepStream copy/release hooks must be exception-safe,
with no pointers into engine scratch memory or model-owned label buffers. The tracked
tee and Program preview consume immutable data; cropper uses the matching transform
without rewriting the source payload. OOM/pool failure must be explicit and bounded.

## Colors and GPU drawing

Use one 32-color palette chosen for contrast on ice and a per-stream lease allocator.
Reserve current-visible leases first and keep inactive leases only while capacity
allows. Before sharing, reclaim the oldest inactive lease. Up to 32 visible players
therefore have distinct colors; with more than 32 visible players, sharing a least-used
color is the explicit finite-palette fallback. Shared overflow leases are temporary:
when visibility falls back to 32, move overflow owners to free slots while preserving
established owners, even if an unrelated color was the one released. ID modulo is not the
allocation policy. The same lease colors boxes, skeletons, and labels in all views.
One producer owns color state: the existing vpplaytracker snapshot owner when present,
or an explicitly requested post-tracker/post-analytics pre-crop metadata producer when
vpplaytracker is absent. Use the latter point for the tracked preview tee as well.
Allocate no separate per-renderer leases and do not put color ownership in TRT state.
Run bookkeeping only when boxes or analytics drawing requests colors; it must not
instantiate inference. The existing owner can respond to preview box toggles; a fallback
producer is created only for a requested drawing capability at graph construction. Untracked detections have a neutral color and no lease.

Program overlays draw on the already owned output surface after crop/rotation, using
batched bounded primitives. A small analytics-specific compositor bins at most 8192
commands into sparse tiles and assigns each pixel one writer, blending in command
order. This prevents overlapping primitive races without one launch per bone. Preview overlays use the existing GPU renderer and the same
coordinates/colors. Extend transform attachment so analytics does not depend on old debug
snapshot selection. Honor packed 10-bit surface formats explicitly; never reinterpret
them as uchar4. The ordinary cropper currently negotiates RGBA8; testing a packed
10-bit renderer alone does not establish end-to-end 10-bit pipeline support. Text rendering uses a cached atlas and bounded glyph commands. Drawing
disabled means no atlas initialization, metadata scan, command-vector allocation,
command upload, kernel launch, or extra surface. Configure the fast path once. A baked
layer mask prevents Program preview drawing the same layer already present in its
video texture; Stitched preview still draws those layers independently.

## Optional nvtracker ReID

Expose a next-run enable setting plus an explicit ReID model configuration. With the
setting off, leave the selected existing tracker configuration and runtime untouched.
With it on, create an owned runtime low-level YAML derived from the chosen tracker
config, overlaying only the validated ReID/reassociation settings and preserving unrelated
NvDCF tuning. Do not copy DS9's entire accuracy template onto DS7.1. Resolve model paths
against their source configuration, use a writable GPU/SDK-specific engine cache, bound
gallery size and extraction cadence, and keep embedding-export metadata disabled.
Require a prebuilt compatible ReID engine and remove rebuild inputs from the derived
configuration: enabling this extension never starts engine building during playback.
Leaving a user-supplied low-level config untouched may preserve its own independently
enabled ReID; the new off setting does not override explicit existing tracker policy.
Reject tracker/model/platform combinations that cannot honor the requested mode.

All calibration-only, timed-mask, player-selection-scan, and INT8-sampling graph
preparations suppress analytics before model discovery. An enabled inference graph
requires primary detection and native object tracking. Missing required inference
resources fail the requested run; diagnostic metadata/command overflow suppresses
bounded overlays with counters and must not corrupt video or deadlock teardown.

## Validation and delivery

Unit tests cover disabled config short-circuit, invalid enabled contracts, track reuse,
color exhaustion/release, reset/gaps, consensus/expiry, transforms, metadata copying, and
tensor decoding. GPU tests cover affine crops, 8/10-bit drawing, stream/lifetime cleanup,
real engine parity, bounded batches, and zero frame readback. Pipeline tests verify
actual element/probe omission, no off-path model/cache I/O or metadata traversal,
enabled ordering, seeks/restarts, and matching preview colors. Drawing targets use up
to 32 players with 17 joints and bounded two-line labels at 1080p and 4K.

Build all targets on x86 and Jetson, exercise real recording playback, record model and
drawing benchmarks, and inspect package/runfiles plugin discovery. Changes to module
ownership or pipeline order update AGENTS.md. Deliver at most four stacked ready PRs,
with two independent reviewers per round after each PR opens; fix actionable findings
and repeat until no required fixes remain.
