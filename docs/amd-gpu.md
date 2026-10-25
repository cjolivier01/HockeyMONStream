# AMD GPU port

Status: native HIP kernels, VA/HIP video interoperability and MIGraphX inference
work on AMD. A ROCm build of `hstream-cli` now owns the saved-calibration Program
video path: stereo decode, stitching, detection, rink filtering, geometric
tracking, camera crop, saved scoreboard, GL preview and HEVC/AAC MP4 output. Desktop Program
launch uses the same pipeline; additional preview channels, live controls
and other optional application features remain unfinished. This is
not yet a complete AMD application release.

`src/apps/apps-common/PreviewRouteOwner.*` is portable CPU control code and does
not depend on CUDA, HIP, Qt or X11. The NVIDIA CLI uses it for existing desktop
route activation. It does not add AMD source/Stitched branches or WebRTC;
those consumers and their capability reports remain unfinished. No HIP surface
or pointer is passed to the NVIDIA preview implementation.
`SelectedPreviewStream` and `PreviewTransportFrames` likewise isolate events and
identity without GPU calls; they do not supply an AMD consumer. The independent
`PreviewGpuSurface` prototype accepts CUDA-device NVMM and remains unavailable
for HIP/VA surfaces. Its NVIDIA hardware test is not AMD preview qualification.

## Implemented paths

`--config=rocm` selects HIP for HStream's highlight texture compositor,
analytics overlay compositor, pose preprocessing/decoding, and jersey, action,
and team preprocessing/reduction. CUDA and HIP compile the same kernel sources.
NVIDIA remains the default, including Jetson builds.

The configuration also enables the existing HIP implementations in the pinned
`hm-cupano` and `jetson-utils` dependencies. Both use the discovered ROCm SDK.
Panorama, Laplacian blending, pitched/batched images, and packed RGB10 remapping
are covered by their existing tests. The dependency revisions are unchanged.

`src/libs/gpu/Runtime.h` supplies source compatibility for HStream-owned kernels.
It selects real HIP types and operations; it does not emulate NVIDIA driver
libraries or make HIP pointers valid inputs to DeepStream or TensorRT. Existing
caller-owned streams, device buffers, asynchronous ownership, and compact
inference reductions are retained. No frame readbacks or CPU video-processing
path are added. Tests read pixels back for comparison.

## Build and test

Install a ROCm SDK compatible with the GPU, plus the repository's ordinary
OpenCV, GStreamer, OpenGL, and C++ development dependencies. The portable
targets do not need CUDA, TensorRT, or DeepStream SDKs.

```sh
make amd-kernels
make test-amd-kernels
make test-amd-video
make test-amd-inference

# Equivalent test command; also works without --config=rocm on NVIDIA.
bazelisk test --config=opt --config=rocm --local_test_jobs=1 --test_output=errors \
  //src/libs/gpu:portable_gpu_tests
```

The suite explicitly selects GPU tests, including HStream's detector, overlay, highlights,
pose and team tests and eight tests from the pinned stitching library.
Run GPU tests serially: concurrent CUDA contexts can exhaust device memory even
when each individual test fits.
`semantic_gpu_test` additionally checks Pillow-reference jersey crops and
jersey/action reductions. Its fixture arguments are described by
`scripts/prepare_player_semantic_fixtures.py` and
`src/libs/player_analytics/runtime/SemanticGpuTest.cpp`.

The toolkit is discovered at `/opt/rocm`. Override it with
`--repo_env=ROCM_PATH=/path/to/rocm`. Architectures are discovered with
`rocm_agent_enumerator`, filtered to concrete `gfx` targets, and recorded with
the compiler version in the compilation tool input. For a build machine without
an AMD GPU, or a binary targeting several GPUs, supply the architecture list:

```sh
bazelisk build --config=opt --config=rocm \
  --repo_env=ROCM_PATH=/opt/rocm \
  --repo_env=HSTREAM_AMDGPU_TARGETS=gfx1100,gfx1200 \
  //src/libs/highlights:texture_blend
```

Unsupported architectures fail in the compiler. HIP compilation uses the local
SDK and its sibling tools/device libraries; it is not a remote hermetic
toolchain. SDK headers, compiler version, and selected architectures participate
in the build inputs. Deployments must supply the compatible ROCm runtime and
its loader configuration. Cross-compiling AMD targets with the Jetson toolchain
is not supported.

## VA-API video interoperability

`src/libs/gpu/vaapi/VaHipFrame.*` imports linear VA surfaces into HIP without
mapping or copying their pixels to the host. It retains the GStreamer buffer,
validates exported object and plane extents, rejects tiled layouts and shared
writable inputs, and releases all imported mappings and descriptors. Import
waits for the VA producer. The caller submits work on its explicit HIP stream
and calls `Complete()` before passing the frame to another VA element; destruction
also drains unfinished work. The stream must outlive the import. These CPU-side
fence waits do not transfer video; asynchronous external-fence integration and
import caching remain performance work.

The optional `make test-amd-video` target additionally requires GStreamer 1.24
with the VA plugin and development library, libva development headers, and Mesa
VA drivers. It negotiates linear NV12 and P010 DMA-BUF frames between hardware
decode and encode. The P010 case runs HStream's existing HIP overlay compositor.
Each codec processes 32 frames and checks encoded pixels, untouched background,
timestamps, EOS, shared-buffer rejection and repeated-import descriptor cleanup.
Pixel readback occurs only in the bounded test verifier after re-encode/decode.

`VaVideoReader` provides reusable local H.264/HEVC MP4 decoding with explicit
device, pixel format, queue bound and operation/cancellation timing. It selects
the VA decoder and postprocessor factories by their actual render node. Seek
flushes the current decoder; opening another chapter resets the same elements
through NULL. GStreamer 1.24.2 closes the postprocessor's filter on PAUSED → READY
and only reopens it on NULL → READY, so stopping at READY fails renegotiation.
Samples retain their surfaces across seek, chapter change and reader destruction.
No decoded pixels are mapped. The reader returns file-relative stream timestamps;
`VaVideoPlaylist` supplies chapter offsets and `VaStereoVideo` pairs the cameras.
Cancellation is permanent and checked between bounded waits. GStreamer/driver
calls themselves are not interruptible. Errors retire the reader.

Repeated chapter changes exposed an upstream GStreamer 1.24.2 leak:
`gst_va_dmabuf_get_modifier_for_format` exports a temporary VA surface but never
closes its DMA-BUF handles. On this host that leaked 13 descriptors per restart,
without HIP involvement. The private build below fixes the probe and failed
exports, leaving the installed library, plugins and core unchanged. It builds
private `libgstva-1.0` and the matching VA plugin against the installed 1.24.2
development packages. The plugin also fixes the Main10 profile constant described below. The source
archive is SHA-pinned; the runtime retains its source, patch, build recipe and
license. The recipe deliberately rejects other SDK versions.

```bash
scripts/build_amd_gstreamer.sh /tmp/hstream-private-gstva 8
/tmp/hstream-private-gstva/run /tmp/hstream-private-migraphx/run \
  make test-amd-video
```

The reader hardware test checks exact timestamps, EOS, seeking after EOS,
queued-frame flushing, chapter changes, cancellation, failed-transition
retirement, and retained pixels after reader destruction. Sixteen seeks and
eight chapter changes per codec show stable descriptor counts with the private
library (47 → 47 for H.264; 47 → 46 for HEVC). The same chapter test fails with
the unpatched library (137 → 241). Two synthetic luma bytes are read back solely
to verify retained-frame ownership. A separate descriptor-ownership test injects
a failed driver export and deliberately reuses its closed descriptor numbers:
the probe must leave those new owners open. Successful exports with rejected
layouts must be closed, as must ordinary successful probes. This test fails with
the initial cleanup patch and passes with cleanup gated on export success.
The reader also exercises retirement after a real malformed-container error.
Its cancellation test covers sticky cancellation; interruption during a blocked
driver call is not promised or tested. The composed raw Program qualifier and
CLI use the reusable reader; the audio, desktop and lifecycle qualification
is described below. RGBA/BGRA import layouts are accepted by the
adapter, but only NV12/P010 codec paths are qualified.

## Native AMD inference

`src/libs/gpu/migraphx/MigraphxSession.*` compiles static ONNX shapes with
MIGraphX's native GPU target. It supports FP32 or FP16 operations with contiguous
FP32 bindings. It disables automatic copies, allocates bindings once, and uses
one explicit caller-owned HIP stream. Destruction drains that stream before
freeing model storage. All input shapes and the binding byte bound are explicit;
unsupported tensor contracts fail preparation. The CLI uses the compiled-model
cache described below; low-level callers can explicitly omit caching.

`src/libs/detection/HipDetector.*` composes that session with shared CUDA/HIP
preprocessing and proposal filtering. It accepts GPU RGBA8, packed RGB10, NV12
and P010, plus the panorama engine's half-float RGBA output. YUV callers supply color matrix/range coefficients from their negotiated
video format. Preprocessing uses half-pixel bilinear RGB sampling, explicit
aspect/padding settings, and the model's scale factor. GPU filtering accepts the
shipped YOLO `[1,N,6]` xyxy/score/class output contract. Only valid proposals cross
to the CPU for deterministic, class-aware NMS and inverse letterbox mapping.
There is no frame readback. Metadata storage is bounded by model rows and the
supplied I/O limit. Readback uses two stream waits and at most `N * 28 + 4` bytes
per inference (usually a few kilobytes after thresholding). Frame-level batching,
GPU NMS and asynchronous scheduling are future optimization work.

`make test-amd-inference` requires the MIGraphX headers and runtime in the ROCm
SDK. The tests exercise multi-input FP32/FP16 programs, successive stream-ordered
results, teardown with queued work, invalid contracts, RGB/YUV preprocessing
against OpenCV float bilinear sampling (including varying chroma), and malformed
proposal filtering. A fault-injected SDK enqueue error verifies that queued reads
of borrowed image planes finish before `Detect()` returns an error. The
CPU `//src/libs/detection:postprocess_test` additionally checks padding removal,
class-aware NMS, output bounds and deterministic score ties.

`//src/libs/detection:hip_detector_fixture` is an offline qualification tool. Its
arguments are `MODEL IMAGE INFER_YAML fp32|fp16 WIDTH HEIGHT IO_BYTES OUTPUT_CSV
RUNS`. It uploads one CPU-decoded image and writes only detection metadata; this
bounded fixture path is not used for production video. On one real hockey
fixture with the shipped 1408×544 small detector, native AMD FP16 and CPU ONNX
FP32 both return 23 players. Matched box IoU is at least 0.9534 (mean 0.9834), with
maximum confidence difference 0.0113. Eight native runs return identical results;
warm preprocessing/inference/filtering/readback/NMS takes 4.4–4.7 ms on the RX
9060 XT. This single-frame check does not qualify full-game detection or tracking.

## Tracking and shared Program policy

`src/libs/tracking/NativeTracker.*` runs two-stage geometric association over the
compact detection metadata, using HockeyMON's native Kalman filter without its
Torch/CUDA wrapper. High-score observations can recover retained tracks;
low-score observations maintain only previously observed tracks. Assignment
memory, track counts, confirmation and retention are explicit caller settings.
Only current measured boxes are published. Seeks require reset, and reset never
reuses a track ID.

`ProgramTracking.*` composes the shared rink filter, this tracker and the existing
native camera policy. It requires an identity-validated mask in the same geometry
as the incoming observations; mismatched dimensions fail. The application must
supply resolved configuration. `ProgramCameraConfig.*` and
`detection/RinkDetectionMask.*` are also used by the NVIDIA plugins, so their
configuration, upper/lower rink rules and camera arena do not diverge.
`make test-amd-tracking` covers association, lost tracks, bounded resources,
mask pruning and frame-by-frame parity with the existing native camera engine.

`//src/libs/tracking:hip_video_tracking_fixture` accepts
`MODEL VIDEO INFER_YAML WIDTH HEIGHT IO_BYTES START_SECONDS FRAMES OUTPUT_CSV`.
It hardware-decodes an already-stitched HEVC archive to VA P010, imports linear
DMA-BUF planes, runs HIP/MIGraphX detection and writes native track metadata.
The appsink advertises video layout metadata during allocation; without that
GStreamer cannot allocate DMA-BUF output. No video pixels return to the CPU.
`VaDetectorImage` requires negotiated color matrix/range and rejects PQ/HLG until
an explicit tone-mapping stage is supplied.

On the RX 9060 XT, 120 frames of an 8192×2356 Main10 hockey archive produced
1,200 confirmed observations across 15 IDs, with strictly increasing timestamps.
After model preparation, the measured loop took 1.1–1.7 seconds. This fixture
uses explicit test tracker settings and does not yet run mask pruning, camera
cropping, original-camera stitching, preview or output encoding. The composed
Program metadata path is currently qualified by separate CPU tests.

## MIGraphX and Mesa library isolation

The ROCm 6.4.1 MIGraphX GPU module exports its embedded LLVM symbols, and its core
loads plugins globally. Loading Mesa's different LLVM in the same process then
aborts with duplicate command-line option registration. Changing library order
alone only reverses which LLVM implementation fails. This affects combining
inference with video/graphics; isolated inference tests do not expose it.

The pinned source recipe `scripts/build_amd_migraphx.sh` rebuilds only MIGraphX's
2.12.0 core with `RTLD_LOCAL | RTLD_DEEPBIND` for its private plugins. The existing
ROCm GPU module, compiler and headers remain unchanged. This is a source change,
not binary rewriting or driver-path preloading. It is currently restricted to
the exact ROCm 6.4.1 MIGraphX SDK identity; another release needs qualification.

On Ubuntu 24.04, preparation needs CMake, Ninja, GCC, curl, patch, the installed
ROCm development packages, `libmsgpack-cxx-dev`, `nlohmann-json3-dev`,
`libsqlite3-dev`, `libprotobuf-dev` and `protobuf-compiler`. Supply a new private
prefix outside the installed SDK; the script refuses to overwrite a prefix:

```sh
scripts/build_amd_migraphx.sh /opt/rocm "$HOME/.cache/hstream/migraphx-6.4.1-local" 8
"$HOME/.cache/hstream/migraphx-6.4.1-local/run" make test-amd-driver-interop
```

The generated `run` launcher selects the private core before process startup.
Bazel tests need `--test_env=LD_LIBRARY_PATH` to retain that selection and
`--test_env=GST_PLUGIN_PATH_1_0` for the private VA plugin; the Make
target above includes it. The two hardware tests initialize MIGraphX, VA and EGL
in opposite orders and render a synthetic OpenGL framebuffer on the selected
HIP device. Their only pixel readback is four test bytes. The real 120-frame
video fixture and the multi-input FP32/FP16 session tests also pass with this
private core. System libva, Mesa and ROCm libraries are never modified. Packaging
and CLI/UI runtime discovery still need to incorporate this qualified runtime.

## Application work still required

| Boundary | Current implementation | Remaining AMD work |
| --- | --- | --- |
| Decode and encode | VA H.264/HEVC stereo readers and HEVC/MP4 output in the ROCm CLI | Additional application sink modes |
| Allocation and pairing | Bounded VA DMA-BUF pools, HIP imports and exact stereo clock | Broader codec/device qualification |
| Stitching | Saved calibration with HIP `hm-cupano` and shared conversion | Automatic calibration, rotated generations and live controls |
| Model execution | Native MIGraphX inference in the ROCm CLI | Broader numerical qualification and optional analytics |
| Tracking | Native geometric association, shared rink filter and camera policy | Appearance reassociation and telemetry-backed seek history |
| Crop and preview | Shared HIP crop into VA output; native-window EGL Program preview with tracked-player boxes | Source/Stitched channels and Play/Rink diagnostics |
| Desktop, jobs and publication | Saved Program playback and managed Program/4K archives | Additional live controls, exported job launch and highlight export |

Do not pass `--config=rocm` to `//...` as an application build: many targets
still directly depend on NVIDIA libraries. Compute tests do not establish
end-to-end AMD playback or export support.

## Hardware evidence

On the RX 9060 XT (`gfx1200`, ROCm 6.4.1), the shared portable suite passes.
Pose preprocessing reports zero error against OpenCV; the additional sixteen
Pillow jersey crops have zero pixel mismatches and zero normalized error.
Overlay checks cover RGBA8, RGB10A2, I420, P010, clipping, row padding, guard
bytes, bounded uploads, and asynchronous ownership. Highlights cover
transparent/opaque texels, premultiplied interpolation and invalid arguments.
HIP validation uses an intentionally nonexistent `CUDA_PATH`; overlay and
stitching binaries load no CUDA, TensorRT or DeepStream libraries.

ZLUDA v6 (`3fe12063`) initializes this GPU and creates a CUDA context. Calling
the installed NVIDIA decoder's `cuvidGetDecoderCaps` for HEVC then returns 101
(`CUDA_ERROR_INVALID_DEVICE`). Substituting `libcuda` does not make the current
video graph usable. ZLUDA is not a dependency of this native HIP implementation.
AMD VA-API independently reports H.264, HEVC/Main10 and AV1 decode/encode
capabilities on this machine; codec capabilities do not validate the missing
surface integration.

## Program pixels and output

`src/libs/program/` now owns the existing crop geometry, bilinear inverse
rotation, shadow lift and exposure. The NVIDIA adapter delegates to the same
kernel compiled by NVCC or HIP. Its x/output-width sampling, black boundary,
alpha and disabled-tone rounding behavior are retained. Detector and Program
sampling share GPU image descriptors and RGB/NV12/P010 conversion; descriptors
validate dimensions, pitch, alignment and address overflow before submission.

Program can write RGBA, RGB or gray pixels, or directly produce NV12/P010.
The YUV path retains floating-point samples through tone evaluation, quantizes
once and averages chroma over each 2x2 output block. Output caps must specify
centered chroma. Matrix/range are explicit; the caller must preserve source
primaries and transfer or convert them separately. HDR is rejected by the VA
adapter. Eight YUV format/range/matrix cases and eighteen packed-output
format/rotation/tone cases compare against scalar pixel references.

`VaOutputPool` creates bounded linear VA-owned DMA-BUF buffers on the selected
HIP device's PCI render node. Acquisition is nonblocking when downstream owns
all buffers. Import an exclusively owned output through `VaHipFrame`, submit
the crop, and complete its stream before handing the original buffer to encode
or GL. The pool and kernel never map video to CPU memory. Tests also retain an
output buffer beyond pool destruction and import it successfully.

The NV12/H.264 and P010/HEVC output tests each encode/decode 32 alternating crops
at 25 fps. They check pixels, exact content times through the encoder's shifted
segment, EOS and bounded pool reuse. This uses an integral nanosecond frame
period; a separate 30 fps probe showed the parser accumulating sub-nanosecond
frame-period truncation (1 ns by frame 3), not lost frames. Pixel readback is
limited to the synthetic 384x192 test output.

The two headless preview tests feed those buffers through
`glupload -> glcolorconvert -> GLMemory` on the HIP device's EGL/GBM display.
They inspect four synthetic framebuffer bytes and reach EOS. These tests qualify
the GL memory handoff, not desktop embedding, resize, pause or tab behavior.
`make test-amd-video` includes both encode and preview cases.

GStreamer 1.24.2's VA format table advertises RGBA as DRM `RA24`, while the
allocated surface exports the correct `AB24` layout. Direct YUV output avoids
that RGB VPP negotiation bug and its extra conversion pass without patching
the RGB format table. The private VA build above is still required for descriptor
cleanup and Main10 signaling. The canonical render-node path
is also required: VA factories compare their path with the context verbatim,
so passing a PCI symlink alias can trigger an error while starting elements.

These tests qualify components. The CLI integration described below composes
raw-camera pairing/stitching and saved Program startup. Later sections describe
model caching and desktop Program routing. Additional preview channels and
installed runtime discovery remain pending.


## Raw-camera Program qualification

`//src/libs/program:hip_program_fixture` composes two hardware HEVC readers,
linear P010 imports, HIP conversion/stitching, MIGraphX detection, saved rink
pruning, native tracking/camera policy, Program crop and HEVC/MP4 output. Its
arguments are:

```
GAME MODEL INFER_YAML CAMERA_YAML LEFT RIGHT LEFT_SECONDS RIGHT_SECONDS FRAMES
MODEL_WIDTH MODEL_HEIGHT OUTPUT_WIDTH OUTPUT_HEIGHT OUTPUT_MP4
```

It consumes an existing reviewed, unrotated calibration and explicit raw chapter
paths/seek times. Input pairs must remain within half a frame after their explicit
seek offsets. It checks the current Hugin/output generation, loads maps through
the existing locked/pinned artifact snapshot, and requires the saved rink mask
in that exact geometry. It refuses an existing output file. This is a hardware
qualifier with explicit test tuning, not a replacement application runner: it
has no chapter transitions, audio, user configuration mapping, seek commands,
archive publication, or desktop integration.

`ConfigureStitchingCore` exposes the shared CPU control plane without NVIDIA
surfaces. `ConfigureStitchingNv` adapts the existing NVIDIA entry points through
lazy, bounded calibration image readers/writers, preserving validation before
readback. The artifact locking, generation and publication algorithms are shared.
`StatusMacros` and `utils_base` no longer pull GPU headers into CPU callers.
On AMD, optional scoreboard still suggestions use the installed ONNX CPU
provider; that path is separate from production video inference.

`gpu/ConvertImage` writes every decoded pixel, including image edges, into IEEE
binary16 RGBA in the panorama engine's 0–255 units. Detection and Program crop
sample this format directly. No intermediate 8-bit frame or GPU-to-CPU copy is
needed. Seven source formats, pitch/tail guards, odd and single-pixel dimensions,
overlap rejection and half-float detector/crop numerics pass on HIP and CUDA.

The installed GStreamer 1.24.2 VA encoder omits color and chroma-location fields
from HEVC VUI. Container color tags alone do not fix readers that prioritize the
bitstream. `program/EncodedColorimetry` applies libavcodec's H.264/HEVC metadata
bitstream filters before parsing/muxing. It preserves the actual encoder input's
range, matrix, transfer and primaries and marks centered output chroma. Only
bounded compressed access-unit bytes are copied on the CPU; there is no video
decoding, pixel readback or re-encoding. Hardware tests independently parse the
resulting SPS and compare every encoded slice payload and timestamp before and
after the filter. Invalid caps, unknown color, malformed packets and packet-bound
violations fail with a bus error.

On RX 9060 XT, the `vegas-calheat-1` raw 7680×4320 camera pair at 60 seconds produced
120 frames through a validated **15632×4100** panorama and a **3840×2160 Main10**
Program export. There were 2,382 confirmed track observations and 119 camera-led
crops after the initial full-canvas frame. The warmed loop took **9.40 seconds**
for 2.002 seconds of source video, excluding model preparation. This configuration
is not real-time. `ffprobe` verifies all 120 frames, 60000/1001 fps, centered chroma
and full-range BT.709 matching the raw inputs. Bounded offline thumbnails were
inspected after export; the processing loop never reads video back.

The CLI and desktop now compose these components with cached model preparation,
audio and managed archives, as described below. Automatic calibration remains
unavailable on AMD; playback requires saved reviewed calibration.

## Shared Program configuration

`common/ConfigPath` resolves and updates YAML paths without depending on GPU or
DeepStream headers. `tracking/ProgramCameraMapping` materializes the native camera
document from the effective canonical configuration and its provenance ranks.
The NVIDIA configurator delegates to this same mapping: baseline, user, game and
launch precedence, native same-layer ties and live-box role validation are
preserved. The mapping does not load or merge application configuration itself.

`runtime.amd` in the shared baseline owns AMD GPU selection, decoder/output pool
bounds, operation deadlines, cancellation polling, inference/packet byte limits,
stitch workspace selection and geometric tracking tuning. `AmdProgramSettings`
requires every value from the effective document and validates integer ranges,
timeout conversion and association settings before GPU preparation. It does not
select an application backend or change NVIDIA/Aspen behavior. The raw Program
qualifier now consumes these settings; its input/model paths and codec/output
choices remain explicit qualification arguments. The ROCm CLI resolves application
configuration and owns these components for both direct and desktop playback.

## Reusable Program processor

`program/AmdProgramProcessor` is the synchronous compatibility facade used by
the raw-camera qualifier. It composes independently owned `AmdStitcher`,
`AmdDetector`, `ProgramTracking` and `AmdProgramRenderer` stages. The normal CLI
uses the GStreamer elements described below. `Process` consumes a paired
VA sample from each original camera and returns compact observation metadata;
`Render` can draw the completed GPU panorama repeatedly into exclusively owned
NV12/P010 output buffers at caller-selected sizes. A full-panorama render uses
the existing no-crop transform. Input/output transfers and primaries must agree;
this stage converts YUV matrices/ranges but is not a tone mapper.

The owner pins and validates saved control maps and the exact-generation rink
mask before GPU preparation. It currently requires the current unrotated saved
geometry; calibration, post-stitch rotation and reconstruction on changed
source caps remain application work. The caller owns source identity, stereo
pairing, timestamps, output pools and decoder/sink lifecycles.

`Reset` clears camera/object histories and the last renderable frame after a
seek, preserving model compilation and stitch allocations. Object IDs are not
reused. Processing failures retire the owner; a reset cannot revive failed GPU
state. Input imports fence their stream on every exit, and destruction drains
work before freeing workspaces and the detector's stream. No full video frame
returns to CPU memory.

Append `--verify-reset` to the raw qualifier command to exercise reader seeks,
processor reset, monotonic IDs, repeated Program/full-panorama renders, rejected
transfer changes and failure retirement. On the 120-pair hockey clip, all 120
per-frame detection/tracking/crop log rows match the earlier composition. This
facade and the ROCm CLI GStreamer graph share the same stage implementations.

## Chapter video clock

`media/Mp4VideoTrack` reads ordinary local MP4 video headers with libavformat. It
never calls `avformat_find_stream_info`, opens a decoder or launches a process.
Video duration, start time, frame count and rate come from the video track; an
audio tail cannot move the next chapter's origin. Unknown/overflowing timing,
ambiguous video tracks, incompatible FFmpeg ABI and unsupported codecs fail.
Preparation has an explicit deadline and cancellation callback.

`gpu/vaapi/VaVideoPlaylist` owns ordered chapters and one reusable VA reader.
It verifies stable source identity during discovery and before chapter opens,
requires matching codec/geometry/frame rate, and exposes a continuous video
clock separately from each retained sample's unchanged file segment/PTS. Seeks
at a boundary select the next chapter; seeking exactly to the end produces
clean EOS and still permits subsequent seeks. Errors retire the reader and
cancellation is permanent. The raw Program qualifier and ROCm CLI use this owner through `VaStereoVideo`;
application audio now follows this video clock through `media/AudioPlaylist`.

The VA reader consumes non-selected compressed streams through closed valves.
Leaving audio unlinked caused a demuxer error when its tail outlasted video;
using an ordinary sink on the demux thread can instead block preroll before a
reordered video decoder has enough input. Closed valves consume those packets
and events without decoding, buffering or waiting for preroll. They are removed
on each NULL chapter reset. Missing selected video fails immediately. GStreamer
1.24.2's qtdemux can suppress video shorter than 20% of the container as a
thumbnail; such inputs fail explicitly rather than timing out.

The metadata CPU test and VA playlist test create small synthetic files with
B-frames, a 0.2-second video and 0.5-second audio, including a 0.4-second starting
edit. Three chapters produce exactly fifteen frames at 25fps on a 0.6-second
video clock. Tests cover boundary/backward/end seeks, retained pixels, source
replacement, cancellation and twelve additional chapter switches without
file-descriptor growth. Only the test reads one synthetic luma byte. Fixture
preparation uses `/usr/bin/ffmpeg` with libx264/AAC; production playback remains
native and uses VA decoding.

`ProgramLaunchConfig` resolves the shared baseline, per-user overlay, saved game,
native application underlay and ordered launch option groups before GPU work. It
loads camera and inference selections using the declaring layer's reference
roots, applies the shared canonical camera mapping in memory, and preserves
ordered camera chapters and fractional synchronization. Missing/invalid saved
inputs fail startup. `ConfigLayers` now supplies NVIDIA's merge, provenance and
camera-reference primitives as well. This prepares application startup; it does
not yet select an AMD CLI/UI backend.

Configuration validation: all four focused HIP-build checks and four CUDA-build
checks pass, including native configurator persistence. The full local x86 build
passes. Both startup configuration reviews found no necessary fixes. The Jetson
attempt still lacks `aarch64-linux-gnu-gcc-11`, `stubby` is unreachable, and native
SBSA requires an ARM64 host.


The Program qualifier also accepts saved application configuration:

```bash
/tmp/hstream-private-gstva/run /tmp/hstream-private-migraphx/run \
  bazel-bin/src/libs/program/hip_program_fixture \
  --configured vegas-calheat-1 configs/ds_hockey_app_config.yaml \
  60 120 3840 2160 /tmp/program.mp4 \
  --options=pipeline.primary-gie.config-file=config_infer_yolov8_hockey_fp16.yaml \
  --verify-reset
```

The positional controls are game, application YAML, synchronized start seconds,
frame count, output width/height and a new output filename. Ordered `--options`
groups use native launch precedence. The selected YAML supplies model precision,
preprocessing, thresholds and declared checksummed assets. `ImageInput` reads the
ONNX input name and static RGB dimensions without loading tensor payloads or an
inference runtime. Unsupported preprocessing, class/ROI filters, dynamic spatial
shapes and prebuilt TensorRT precisions fail explicitly. Saved camera settings,
chapter lists, fractional synchronization and blend settings are consumed in
memory; no generated runtime YAML is published.

`StereoFramePlan` selects the left frame at/after the requested start and the
nearest right frame, with deterministic half-frame ties and bounded decimal
rounding correction. `VaVideoPlaylist::SeekFrame` resolves those indices within
each chapter. A global rational timestamp alone is insufficient: three five-frame
30 fps chapters accumulate a one-nanosecond duration rounding difference, and an
accurate seek can otherwise return the previous picture for one nanosecond.
Frame-index seeking combines the original MP4 origin and integer frame offset
on the track clock before converting to nanoseconds. This also prevents an
extra one-nanosecond frame when the starting edit is fractional. It requires
constant-rate metadata and integral frame ticks. Header rates are only a hint:
frame-mode reads also validate actual presentation timestamps, durations and
chapter frame counts. A variable-rate input with misleading constant-rate
headers fails before processing. GStreamer B-frame segment conversion can
produce a one-nanosecond tail of the preceding picture after a seek; the reader
discards at most one such prefix and validates the following full frame.
Tests cover fractional
starts/offsets, ties, nonintegral frame periods, chapter boundaries, fractional
origins and EOS.

The private VA plugin corrects another GStreamer 1.24.2 defect: its
`VAProfileHEVCMain10` branch sets `GST_H265_PROFILE_IDC_MAIN`, producing ten-bit
pixels with a contradictory Main profile label. The pinned patch changes the
constant to Main10. The output test now checks the SPS profile, compatibility
flags and both bit depths, in addition to colorimetry and unchanged compressed
slices. It fails with the original plugin and passes with the private plugin.
Earlier recordings described above had ten-bit pixels but that incorrect profile
label; rebuilt output reports both `Main 10` and `yuv420p10le`.

The configured qualifier and normal ROCm CLI share these components. Desktop
routing, overlay controls and installed AMD discovery
remain integration work.

Configured validation on the RX 9060 XT: 120 raw stereo pairs from
`vegas-calheat-1` at 60 seconds produced 2,719 confirmed track observations and
119 camera-led crops in 8.17139 seconds, excluding model preparation. The
3840×2160 HEVC file reports Main 10, yuv420p10le, 60000/1001, full-range BT.709
and centered chroma. Seek/reset, repeated renders, increasing IDs and failure
retirement pass. All 17 focused HIP tests and seven CUDA-build tests pass, as
does the full local x86 build. Jetson validation still lacks the cross-compiler,
`stubby` is unreachable, and the SBSA build requires a native ARM64 host.

Review closure for configured startup: rounds one through six corrected asset
path/snapshot consistency, unsupported detector options, alias precedence and
frame-clock rounding. Round seven reproduced misleading VFR headers and
B-frame demux rounding; per-read grid/count validation and bounded prefix
discard address both. Both round-eight reviews found no necessary fixes.

`gpu/vaapi/VaStereoVideo` now owns both camera playlists. It selects frame
indices from saved fractional offsets, advances both sources together, resets
sequence numbers after a seek and ends at the shorter synchronized recording.
A half-failed seek retires both readers. The output clock comes from the frame
index and saved origin, so chapter duration rounding and audio tails cannot
shift Program time. Retained VA samples survive seeks and owner destruction.
The configured qualifier and AMD CLI use this owner. Desktop seeks use its reset
contract, as described below. Hardware tests cover fractional offsets, both
left- and right-limited EOS, 25/30 fps chapter transitions, retained pixels,
backward/end seeks, partial seek failure and cancellation.

The same 120-pair 4K Program qualification with the stereo owner produced
2,719 confirmed track observations and 119 crops in 7.50771 seconds after
preparation. Full x86 build and the three focused source/clock tests pass.
The first stereo review found a tiny-negative-time edge when a saved offset
rounds to an integer frame; the synchronized clock now uses the planner
rounding tolerance at zero, with a passing regression.
Both second-round stereo reviews found no necessary fixes.

`program/VaProgramSink` extracts output ownership from the raw Program qualifier.
It allocates a bounded pool of linear VA surfaces, waits for reuse with explicit
deadlines/cancellation, and routes completed GPU frames to discard, GL preview
or HEVC/MP4 output. The encode caller supplies an empty regular file descriptor
and owns naming, exclusive creation and publication; the sink duplicates that
descriptor and requires successful mux EOS before completion. Cancellation and
errors never report successful completion. Preview accepts a native window and
an optional GL display context. No video pixels are mapped or copied to host.

Hardware tests cover five-frame NV12 and P010 HEVC files, duplicate descriptor
ownership, repeated EOS, post-EOS rejection, nonempty-file rejection, timestamp
overflow, exhausted-pool timeout, concurrent cancellation of a blocked acquire,
retained surfaces after destruction and headless EGL/GL preview. The configured
qualifier and AMD CLI use both the stereo and sink owners. Desktop routing,
pause/resume and local seek behavior are described below.

Output review found that the generic VA encoder factory can select another
GPU on a multi-device host. `gpu/vaapi/VaElement` now shares the reader factory
selection with output, matching the pool's canonical render node and refusing
unknown devices. A one-surface GL preview also stalls while retaining its last
picture; preview now requires capacity for the displayed frame and replacement.
Tests verify selected-node identity, no fallback for an unknown node, rejected
one-surface preview and cancellation during a clocked EOS wait. The first real
120-pair run with the output owner closed its 4K Main10 MP4 in 8.18391 seconds
after preparation, with the same 2,719 track observations and 119 crops.
All nine focused output/source hardware tests and the full local x86 build
pass after the fixes. Both second-round output reviews found no necessary
fixes. Jetson still lacks its cross-compiler and reachable host; native SBSA
requires an ARM64 machine.

## AMD CLI integration

`--config=rocm` selects `src/apps/hstream-cli/AmdPipelineApp.cpp` for the existing
`hstream-cli` target. CUDA builds retain `PipelineApp.cpp`. The AMD entry point
loads immutable baseline/user/game/application/launch configuration and requires
saved source chapters, synchronization, calibration and rink mask. It owns one
`VaStereoVideo`, a componentized `AmdProgramGraph` and one bounded `VaProgramSink`
per requested output. No steady-state video readback is added. SIGTERM
cancels source/sink waits after preparation; SIGINT drains completed output.
During model compilation the process
retains ordinary signal termination. A desktop parent identity is checked with
`PR_SET_PDEATHSIG` before startup. GLib output and errors join the existing bounded
process diagnostics.

`ProgramOutputConfig` resolves dimensions, bitrate, playback start, preview
clocking, crop, edge rotation and tone. AMD-specific defaults live exclusively
under `runtime.amd` in the shared baseline. Canonical output settings and native
property aliases respect layer precedence; a single canonical dimension preserves
the baseline aspect unless the counterpart was explicitly configured. Tone is
applied to the Program output after detection. This does not yet reproduce
pre-inference tone changes from the native high-bit stitching path.

The command supports saved Program playback with detection, tracking, scoreboard
and source audio. `AmdProgramAdmission` rejects enabled analytics, diagnostic
overlays, telemetry and other known unconnected stages before preparing the GPU.
It also rejects disabling a mandatory Program stage. The example disables
scoreboard so games without a saved polygon can run:

```bash
bazelisk build --config=opt --config=rocm //src/apps/hstream-cli:hstream-cli
HM_CONFIG_ROOT="$PWD/configs" /tmp/hstream-private-gstva/run \
  /tmp/hstream-private-migraphx/run bazel-bin/src/apps/hstream-cli/hstream-cli \
  -g GAME -c configs/ds_hockey_app_config.yaml \
  --enable-sources=URI-MULTIPLE --enable-sinks=FAKE --start-time=60 -t=2 \
  --options=pipeline.hmplaycropper.show-scoreboard=0 \
  --options=pipeline.primary-gie.config-file=config_infer_yolov8_hockey_fp16.yaml
```

Use `RENDER` for a GL preview, optionally with `--render-window-id=XID` and
`--render-realtime`. Use `ENCODE_FILE` for the Program archive and
`ENCODE_PROGRAM_4K_FILE` for an independent copy fitted within 3840×2160.
`--options=video_out.output_video_path=/path/to/new.mp4` selects an explicit
Program filename. Sink names may be comma-separated. Rendering needs an available
display; a headless host
can still run FAKE/ENCODE_FILE. Private runtime launchers remain required on the
qualified host; package discovery and installation are pending.

`ProgramFileOutput` holds an unnamed file on the destination filesystem. Only
after every sink reports EOS does it flush the file and create the final name
without replacement. Errors, cancellation and process death leave no partial
MP4 at that name. The directory is pinned for the run. This staging path requires
Linux `O_TMPFILE` and `/proc/self/fd`; unsupported filesystems fail before GPU
preparation. Managed runs register the completed file with the existing archive
publisher, described below. An error flushing the directory after linking
reports that the completed working file is already named.

The CLI accepts one application YAML and rejects two-stage calibration, unknown
arguments, unsupported sinks and malformed time/preview arguments. Time limits
advance from completed Program frames. Additional desktop preview channels, interactive scoreboard selection, analytics overlays,
telemetry, managed jobs and automatic calibration are still integration work.
Tracker association uses the explicit AMD geometric settings; it does not run
NvDCF's appearance model. Compiled-model reuse follows `runtime.amd.model_cache`.

The initial normal CLI runs processed 120 frames and 2,719 track observations
from `vegas-calheat-1` at 60 seconds. Encoded output was 3840×2160 HEVC with 120
frames and a 2.002-second stream duration. These runs qualify the core video
composition, not desktop or audio behavior. CPU startup tests cover argument
rejection, feature admission, configuration precedence and publication on success,
cancellation, SIGKILL and a conflicting destination.

CLI review closure: round one fixed duplicate leveling rotation, missing feature
aliases/native stages, native disable precedence and cancellation during the
publication flush. Round two added mandatory stitching and calibration-only mode
checks. Round three rejected unsupported post-stitch rotation requests; round
four preserved explicit null clearing across layers. Both fifth-round reviews
found no necessary fixes. All six focused HIP and eight native startup/config
checks pass after the fixes, as does the full local x86 build. The corrected
120-frame CLI export retains 2,719 track observations, Main10/full-range BT.709
and centered chroma. An offline 960-pixel still was inspected; that bounded
validation readback is outside playback. The synchronized baseline companion is
HockeyMON PR #219 at `c978e984`; existing default values remain unchanged.

## Saved scoreboard on AMD

`ProgramScoreboardConfig` resolves the enabled flag, saved polygon, dimensions,
scale and placement by configuration layer, including native property bags. The
shared baseline now explicitly supplies `rink.scoreboard.position: upper-left`.
Disabled/all-zero polygons allocate nothing. Enabled overlays require a saved
polygon and its `rink.stitched_output_generation`; preparation compares that
identity with the processor's loaded generation before producing frames. Missing
or stale geometry fails startup. Interactive selection and live placement/reselect
commands still need desktop integration. Current AMD sampling requires bilinear
filtering and zero sharpening; other requests fail explicitly.

`scoreboard/ScoreboardPlan` preserves the native least-relative-change aspect
adjustment and left/center/right placement. It validates a convex polygon inside
the current panorama and a finite projective mapping without a pole in the
output rectangle. `RenderProgramYuv` samples that rectangle directly from the
owned panorama while producing NV12/P010 Program output. Original video alpha
cannot erase the overlay. The scoreboard follows the native ordering after
Program crop/tone and therefore samples the original panorama's colors. There
is no extra video allocation, upload, download, map, file access or stream wait
per frame. The direct homography uses one bilinear reconstruction, whereas the
NVIDIA scoreboard first resizes a cropped image and then warps it; subpixel
values are not claimed identical to that two-pass reconstruction.

GPU tests use a half-float synthetic panorama and independently verify every
luma/chroma output value, including mixed overlay/background 2x2 chroma blocks,
zero source alpha, unaffected crop pixels, row padding and guards. They also
reject stale dimensions and nonfinite matrices. CPU tests cover generation
binding, native aliases/layers, disabled sentinel, baseline reset, malformed
polygons, sizing and all placement modes. Existing NVIDIA scoreboard tests
remain unchanged.

The real AMD CLI export with scoreboard enabled completed 120 frames and 2,719
track observations. Its atomic MP4 is 3840×2160 HEVC Main10, 2.002 seconds; a
bounded offline still confirms the saved scoreboard in the upper-left. Five
focused HIP tests, six NVIDIA-build tests (including the local V100 kernel test
and existing scoreboard interpolation regression), and the full x86 build pass.
Both scoreboard reviews found no necessary fixes. ARM/Jetson validation retains
the missing local sysroot/native ARM64 host and unreachable `stubby` limitations.


## Program audio on AMD

`media/Mp4AudioReader` opens only the native AAC-LC decoder; compressed video is
marked discarded. It returns bounded owned float PCM at the recording's native
sample rate, with one operation deadline and sticky cancellation. A seek includes
the preceding AAC access unit, including negative priming timestamps at file
origin, to rebuild overlap state before exposing audio. Mono and stereo AAC-LC
up to 96 kHz are supported; missing/ambiguous audio tracks and format changes
fail. Audio uses a separate MP4 demuxer so its backpressure cannot stall either
camera's GPU decoder. It rereads compressed file data but never decodes, maps or
copies a video surface.

`media/AudioPlaylist` binds chapters to their discovered file identities and uses
the same video-track origins/durations as `VaVideoPlaylist`. It trims audio tails,
fills gaps inside a valid audio track with silence, and rounds each video-clock
boundary once to the nearest audio sample (at most half a sample of error).
The CLI drains it only through the completed pair's end. Seeks discard pending
PCM and rebind the selected chapter; errors retire the owner. Audio enabled
without an audio track is an error, rather than a silently muted output.

`ProgramAudioConfig` resolves generic/numbered native audio fields and underscore
aliases by layer. It currently requires one source-linked block (`src=2`,
`dest=1`, `sink-id=-1`) feeding all requested sinks, from the camera with zero
saved synchronization offset. `runtime.amd.audio` owns decoder/queue bounds and
the AAC output bitrate. Source sample rate/channels come from the file; no
independent resampling default is introduced. `pipeline.hmaudio.enable=0`
disables audio. ALSA source capture and separate audio files remain unsupported.

`VaProgramSink` places audio and video in the same GStreamer pipeline. MP4 output
uses bounded PCM submission, `avenc_aac`, and the existing video mux. A bounded
compressed-video queue covers four AAC access units plus one video packet,
derived from the native sample/frame rates. It releases VA surfaces while AAC
accumulates preroll. Packet size is bounded by the existing encoded-packet guard.
A corresponding compressed-audio queue covers two video-frame intervals (current
picture plus mux lookahead) and two AAC access units. This prevents low-fps audio
from blocking the next video submission. The mux selects the earliest packet
without chunk batching, so it cannot hold audio waiting for a future video chunk
past the bounded producer. Preview uses
ALSA, the configured `alsa-dest-device`, and `HSTREAM_RENDER_AUDIO_MUTED` for the
initial mute. Normal-speed preview shares the system presentation clock (ALSA follows it); an unpaced
preview drops old monitor-only PCM when its bounded appsrc fills. Archive audio
always retains samples. Blocking audio pushes and mux EOS have cancellable
waits. All PCM work is CPU audio work; video stays GPU-resident.

`media/Mp4AudioEdit` now owns the fixed-size MP4 edits previously local to
`HighlightMp4Audio`. Both callers preserve avenc_aac's 1024-sample decoder preroll
and remove it from the audible timeline after mux EOS. The shared implementation
accepts the source rate and exact PCM count, preserves descriptor offset, validates
all metadata before writing, and never touches video payload. The Highlights
wrapper retains its existing 48 kHz/millisecond contract. Publication still owns
the final flush and non-overwriting link.

Focused tests cover AAC priming and seek alignment, audio gaps/chapter tails,
file replacement, watermark boundaries, configuration precedence, blocked-push
cancellation, and 120-frame 60 fps VA HEVC/AAC exports at 48 kHz and 8 kHz with only two VA
surfaces. The export test decodes only
audio to compare its audible phase against the input tone. Headless EGL preview
with a PCM-only wrapper around ALSA's null device exercises clocked audio/video EOS; this does not qualify
physical speaker output. The existing Highlights MP4 regression test remains in
place for both metadata versions and rejection without partial edits.

The real audio-enabled Program export from `vegas-calheat-1` at 60 seconds
completed 120 frames and 2,719 track observations. `ffprobe` reports 3840×2160
Main10/full-range BT.709 video and 48 kHz stereo AAC-LC, both starting at zero and
lasting 2.002 seconds. The run took 109.49 seconds including uncached model
compilation, with 2.16 GiB maximum resident memory. This does not claim real-time
playback. The full x86 build and focused native/HIP tests pass. ARM/Jetson checks
remain blocked by the missing local sysroot/native host and unreachable `stubby`.
The HockeyMON baseline copies match exactly and preserve every pre-existing
value; its Bazel test cannot reach analysis because local Torch requires the
missing `libcupti.so.13`.

Audio review round 1 found low-sample-rate AAC preroll could exhaust the VA pool;
the compressed-video queue fixes that with two-surface regressions. Round 2 found
the opposite low-fps PCM stall; the compressed-audio queue now covers the mux's
next-picture lookahead. A five-second 1 fps / 48 kHz native regression passes,
alongside the 60 fps / 48 kHz and 8 kHz cases. The 1 fps fixture uses intra-only
encoding at 100 kb/s because this Mesa driver emits a filler-only inter-frame
access unit at that rate; the invalid packet is rejected by colorimetry validation.
The scheduling fix does not hide that driver limitation.

## Compiled AMD model cache

`gpu/migraphx/MigraphxCache` retains native compiled programs across CLI runs.
`runtime.amd.model_cache` in the shared baseline enables it, sets the directory
to `{user_cache}/hstream/migraphx`, and bounds each serialized program at 2 GiB.
`{user_cache}` resolves to `XDG_CACHE_HOME` or `HOME/.cache`; an explicit absolute
directory is also supported. Set `enable=false` for uncached preparation.
These settings are resolved before GPU preparation and do not affect NVIDIA.

Every launch parses ONNX and hashes the uncompiled program, including external
tensor bytes and resolved shapes. The key also covers precision, our compilation
contract, GPU architecture/model, HIP versions, kernel release, actual loaded
MIGraphX/HIP/HIPRTC/math-library paths and SHA-256 contents, and relevant ROCm
environment values. Custom `MIGRAPHX_HIP_COMPILER` overrides require disabling
the cache. Library hashing is startup work; no cache I/O occurs on video frames.

A miss compiles and validates bindings before publishing. A hit copies the
bounded program into a private preparation directory, verifies its SHA-256,
loads it, and rechecks the same binding/output contracts. Corrupt or oversized
entries fail visibly; remove the named entry or explicitly disable caching to
recover. No silent recompilation masks a damaged cache. Cache roots/entries
must be private directories owned by the current user, without symlinks.

Program and checksum publish together through an atomic no-replacement
directory rename after file/directory sync. Concurrent misses may compile
independently, but only one complete entry wins. Normal completion/failure
removes preparation directories; SIGKILL can leave a private `.prepare-*`
directory, which can be removed when no preparation is running. There is no
automatic eviction: users may remove old entries while playback is stopped.
The byte limit is per program, not a total cache quota.

## AMD playback runtime controls

The CLI consumes newline-terminated stdin commands between complete audio/video
submissions through `program/ProgramRuntimeControl`. `@process-pause N` and
`@process-resume N` implement the desktop generation protocol: N must be a
positive integer, a new pause advances the generation, and resume must match it.
A successful output state change precedes the `HSTREAM_PROCESS_PAUSE` acknowledgment.
All output pipelines pause their clocks, and the producer waits without any
source/audio/output operation deadline in flight. The desktop may then SIGSTOP
the process until SIGCONT followed by the matching resume command. Preroll can
remain pending while paused; resume supplies the remaining input.

`@set-render-audio-muted 0|1` changes only preview volume and emits the existing
`HSTREAM_RENDER_AUDIO` acknowledgment with its branch count. Archive PCM and
encoded audio remain unchanged. `@quit` or `q` drains completed output like
desktop Stop. SIGINT/SIGTERM also interrupt a paused control waiter.

`runtime.amd.maximum_runtime_command_bytes` bounds both retained line storage
and input work per unpaused service call. Overlong/malformed commands report
an error; a truncated line is never executed at EOF. Ordinary stdin EOF leaves
playback running; EOF while paused cancels because no resume can arrive.
Controls become active after preparation and before the first processed pair.
Queued quit and end-of-recording seeks consume no frames. Model preparation is
not acknowledged as a paused playback graph. Preview-channel routing uses the
generation protocol described below; additional runtime mutations remain pending.

`@seek POSITION_NS GENERATION` seeks local preview/discard runs relative to the
configured playback start. `@seek-relative DELTA_NS GENERATION` jumps from the
last completed playback position, clamping signed jumps to the run horizon; archive outputs,
paused requests, stale generations and positions past the synchronized end are
rejected before graph mutation. The stereo owner selects the exact first pair
at or after the target. Audio seeks to that pair and restarts its sample origin.
Output graphs go through READY to flush queued audio/video and restart at zero,
retaining the VA pool, native window and volume choice. Inference/stitch resources
survive; tracking and camera histories reset. A failed partial seek retires
playback. Seeking exactly the synchronized recording end completes the local run.

Completed frames emit the desktop `HSTREAM_PROGRESS` fields with position and duration
relative to the configured start, remaining time, rate, FPS and generation.
The displayed horizon respects an explicit time limit; absolute source times
remain in `HSTREAM_AMD_FRAME` diagnostics. Position
queries never advance timed runs: the time limit counts completed video across
seek segments. `@reset-progress-rate GENERATION` implements the existing reset
acknowledgment and generation fencing; resume/seek also clear rate history.
The shared rate estimator reports an unknown ETA if an extremely slow rate
would overflow its nanosecond clock, on either backend.

### Desktop build boundary

`//src/apps/hstream-ui:hstream-ui` now builds with `--config=rocm` and
`--repo_env=CUDA_PATH=/tmp/hstream-no-cuda`. Its executable does not link CUDA,
TensorRT, DeepStream or NVIDIA surface libraries. The existing desktop window,
configuration editors and CPU stitching/rink-map helpers remain shared. GPU
memory detection uses HIP in this build and preserves the existing memory-profile
threshold. CUDA builds retain their existing query and optional dialogs.

Camera Experiments and Highlights still own NVIDIA-only video pipelines. Their
buttons are disabled with an explanation in the ROCm build; no placeholder video
backend is substituted. The AMD desktop launcher requires the matching CLI and
preserves the caller's private VA/MIGraphX runtime paths. It does not stage
DeepStream plugins/libraries and uses a separate GStreamer registry. Installed
runtime packaging remains pending.

### Desktop Program preview

The ROCm CLI accepts the desktop's `--ui-preview-windows=CHANNEL:XID,...` and
`--ui-preview-active=CHANNEL|none` protocol. Program is the available video
channel. Selecting a mapped Source/Stitched tab deactivates Program and reports
that channel unavailable; returning to Program requests a fresh frame. The
third source target reserved by the desktop is accepted even for stereo games.
Unknown/unmapped runtime selections fail without disturbing the current branch.
Generations reject stale commands, and startup begins at generation 1 to match
the UI. The optional desktop calibration revision/reference arguments must
match the saved game snapshot; CLI overrides cannot rewrite that snapshot.
Reference times compare through an exact decimal nanosecond parser, so `62.099`
and `00:01:02.099` agree without rounding distinct references together.
`ProgramCalibration` repeats the owner/reference check against
current saved configuration while the processor holds the Hugin artifact-load
lock, and carries the owner into rink-mask loading. A replacement calibration
during source/model preparation therefore fails startup. These arguments do
not trigger automatic calibration.

`VaProgramSink` observes glimagesink's draw callback and fences the GL thread
from the processing owner before announcing `ready`. Submitted buffers and
base-sink render counters alone do not establish that a window frame was drawn.
An activation watermark excludes queued old frames from readiness; seeking
resets that watermark with the output timeline. Disabling fences earlier draws
and draws black on subsequent exposes. Audio mute remains independently
controlled. The CLI selects EGL before creating GStreamer threads because GLX
cannot negotiate the P010 DMA-BUF path. No video is mapped or read back.

`NativePreviewSurface` clears native sink children as well as its own surface
on resize/remap. This includes glimagesink's child window while its process is
SIGSTOP-paused. Ordinary Qt updates preserve the frame; no geometry handler
pumps posted events. The X11 test includes an idle renderer child and verifies
both black clearing and preservation during ordinary updates.

Opt-in desktop qualification uses the actual UI and CLI on an AMD-accelerated
X11 display. Use a private copy of a calibrated game because normal desktop
controls save next-run choices. With `HM_GAME_DIR` selecting that private root,
run the matching `hstream_ui_test` binary with `HSTREAM_UI_E2E_GAME_ID=GAME` and
`HSTREAM_UI_E2E_AMD_PROGRAM=1`, through the private runtime launchers above.
The check exercises Play, Player boxes on/off, Render off/on, an unavailable tab and return to
Program, and requires native readiness plus nonempty track observations.
This does not qualify other tabs, optional live controls, archive jobs or
physical monitor/speaker output. The first model compilation can take longer
than cached startup; a private virtual X11 display and null ALSA device are
sufficient for protocol qualification.

On the RX 9060 XT, the real desktop check processed frames with detected tracks,
acknowledged Render off/on, and restored Program at generation 5 after visiting
the unavailable Stitched tab. A separate 120-frame native-window run covered
stale/unmapped selections, seeking and SIGSTOP-paused resize. Its stopped GL
child retained 10,000 nonblack sampled pixels after a parent-only clear; the
new clear reduced that bounded diagnostic sample to zero. Pixel sampling was
only in the external test driver. The ordinary desktop/CLI path has no readback.

### Program preview player boxes

The desktop Player boxes switch now applies live on AMD. Initial selection uses
the existing `HSTREAM_UI_PREVIEW_OVERLAYS=players` launch contract. Runtime
`@set-preview-overlays GENERATION PLAYERS PLAY RINK` uses the shared strict
parser and generation state; successful requests receive `status=applied`.
Unsupported Play/Rink layers and stale requests receive `status=failed` and
leave the previous selection intact. These two diagnostic layers remain pending.
Team affiliation and the rink mini-map also remain unavailable in the AMD
preview; the mini-map's navy Home and orange Away markers currently use the NVIDIA
preview path.

`ProgramPreviewBoxes` retains bounded CPU metadata and shared track-color leases,
then maps player boxes through the same source/crop/rotation geometry as the
video. Only currently observed players draw; seek clears old tracks and leases.
The processor uses its sparse GPU overlay compositor on the preview's owned P010
output after crop/tone. The panorama and each archive render remain independent.
Output matrix and range are explicit, including full-range video; existing
NVIDIA compositor callers retain BT.709 limited-range encoding. No frame mapping,
readback, extra full-frame image or additional stream wait is introduced. GPU
uploads contain only bounded drawing commands. All-off rendering allocates no
overlay GPU resources; capacity failures suppress and report the diagnostic layer.

### Graceful Program Stop

The desktop's SIGINT and explicit stdin `q`/`@quit` stop at a complete audio/video
frame boundary, resume paused output clocks, and drain every sink through EOS.
A file is named only after that drain and its AAC edit succeed, so a deliberate
Stop retains the completed clip. A queued quit before the first frame produces
no file. SIGTERM, parent death, paused stdin EOF and processing/output failures
still abort and discard the unnamed output. A later SIGINT cannot downgrade a
SIGTERM cancellation. Decoder/output waits retain their configured bounds.

The opt-in `//src/apps/hstream-cli:amd_program_lifecycle_test` runs ten real-camera
cases, including a queued seek to EOS before the first frame, one-frame encode,
normal/paused Stop, conflicting signals and stdin loss. Set
`HSTREAM_AMD_TEST_GAME_ID` and `HSTREAM_AMD_TEST_START_SECONDS` to a saved calibrated
game with audio and at least two seconds remaining, supply `HM_CONFIG_ROOT` and
`HM_GAME_DIR`, and launch Bazel through the private runtimes above. The test
requires ffprobe/ffmpeg to inspect and fully decode the temporary output clips;
those offline checks are the only video readback in the test.

### Managed Program archives

The desktop can save Program and 4K Program together with native preview. Each
branch has a bounded VA pool and renders the same processed panorama directly
into its output size; detection/tracking runs once per pair. Scoreboard plans
are prepared separately for each output, and preview diagnostic boxes never
enter either archive. Native sink IDs, output names and bitrate overrides follow
configuration layer precedence. The 4K branch preserves Program aspect without
upscaling; native per-sink Program scaling must preserve the same aspect.

Automatic bitrate uses the highest video-track bitrate among the saved camera
chapters and scales its per-pixel density to each archive size, using the shared
integer bitrate helper. Metadata comes from the native MP4 header reader without
opening a decoder or subprocess. Explicit native/canonical/AMD bitrates retain
precedence; missing source bitrate retains the configured fallback and reports
that choice. The AMD archive path supports hardware Main10 HEVC in MP4 and
bilinear 4K fitting. Incompatible explicit codec, profile, container, scaling
and interpolation settings fail before GPU preparation. Duplicate output paths
and sink IDs also fail preflight.

A desktop run already supplies `HSTREAM_JOB_MANIFEST`; default output names
resolve inside that private workspace. After every output reaches EOS and AAC
editing completes, the CLI names each file without replacement, registers its
exact open descriptor identity, and marks the manifest encoded. The existing
desktop publisher or `hstream-finalize` remuxes to faststart MP4, publishes the
versioned game-local files and origin sidecars, then removes owned working files.
Failure marks the manifest failed and prevents final publication. A producer
without telemetry support rejects a manifest requesting telemetry at startup;
it never clears that request. DriveGPT capture remains unavailable on AMD.

Without a manifest, ordinary archive names use the configured output-root/game
directory. Explicit Program paths retain their requested location. No output
name is overwritten. Program/4K simultaneous output and the desktop Stop path
can be qualified with `HSTREAM_UI_E2E_AMD_ARCHIVES=1` alongside the existing
`HSTREAM_UI_E2E_AMD_PROGRAM=1` desktop test. Exported `--encode-job` launch remains
unimplemented; the native finalizer can already supervise this CLI directly.

Qualification processed 120 pairs into simultaneous 4096×2304 and 3840×2160
Main10 HEVC/AAC archives. Both fully decode, start at zero, last 2.002 seconds
and retain the 60-second recording offset in their sidecars. A desktop Stop
published both 13-frame clips and cleaned its run directory. The existing
faststart remux uses a millisecond movie/edit clock: those short outputs report
0.216883-second video and 0.217000-second audio. SIGTERM during dual encoding
records a failed manifest with no registered outputs or named MP4s.

## Componentized Program graph

The ROCm CLI now uses the statically registered `hstreamamdprogram` plugin:

```text
paired VA samples → appsrc
  → hmamdstitch → hmamdinfer → hmrinkfilter → hmobjecttrack → hmcameratrack
  → appsink (completed panorama and observations)

completed panorama + exclusive output → appsrc → hmamdcrop → appsink
  → existing VA encoder or GL preview graph
```

These are separate GStreamer elements with pads, bus errors, state transitions
and read-only `processed-frames` properties. Each owns its processing object;
they do not dispatch into a shared monolithic processor. `AmdVideoStages` splits
stitching, detection and rendering; `ProgramRinkFilter`, `NativeTracker` and
`ProgramCamera` own the three metadata stages. The qualifier's older
`AmdProgramProcessor` interface remains as a facade over these libraries.

The application supplies resolved configuration through `CreateElement`; a
factory without configuration refuses READY → PAUSED. The plugin is embedded
in the CLI, not installed as a separately discoverable gst-launch module.
`GST_DEBUG_DUMP_DOT_DIR` captures both actual graphs with element names and caps.
The desktop Pipeline tab is not yet connected to these graphs.

Internal `application/x-hstream-amd-frame` envelopes carry typed `GstMeta` with
retained input samples, an owned immutable GPU panorama and compact observations.
Header copies clone metadata and retain the panorama; render envelopes with an
exclusive destination cannot be copied. Each stage validates its input phase.
Panorama allocations are reused only after every downstream owner releases
them, with the existing `runtime.amd.output_pool_frames` as their allocation
ceiling. The Program graph requires at least two pool frames, including with a
FAKE sink: completion appsinks retain the previous frame until its replacement
arrives. A value below two fails configuration admission before GPU startup;
the configured bound is never silently increased. The baseline remains eight.
Allocation is lazy. GPU reads/writes complete at the stage boundary;
no frame is mapped to system memory. Cropped output advertises linear DMA-BUF
caps. If appsink still holds its completed buffer, the encoder handoff creates
only a writable header, refs the exact existing memories, and pins the original
pool buffer with parent-buffer metadata until the encoder releases it.

The integration still admits one complete stereo pair at a time and renders
outputs sequentially. Source pairing, audio, runtime commands and publication
remain application-owned. The new boundaries permit further scheduling work;
this change does not yet enable multiple frames in flight or promise a speedup.
Seeks pass both graphs through READY, clearing queued envelopes and resetting
tracker/camera histories while retaining compiled inference and stitching state.
Failures retire the graph; cancellation tears it down before borrowed work can
outlive its owners. Driver calls retain their existing interruption limitations.

The tracker remains the tested motion/overlap implementation using HockeyMON's
ByteTrack Kalman filter, high/low-score association and bounded retained tracks.
It is not NvDCF appearance tracking or HockeyMON's complete Torch-based ByteTrack
wrapper. `hmobjecttrack` now provides an independent replacement boundary.

Qualification on `ripper` (RX 9060 XT) completed the same 120 raw stereo pairs
with all per-frame detection/tracking counts matching the previous runner:
2,719 track observations, zero-based HEVC/AAC, 120 frames and 2.002 seconds.
The single 4K archive run reported 16.3 processing FPS and 18.67 seconds total
including preparation; simultaneous 4096×2304 Program and 3840×2160 archives
reported 13.1 processing FPS. Both outputs fully decode. These are short-run
measurements, not sustained full-game benchmarks or a claimed speedup.

All ten real-camera Stop/abort lifecycle cases pass. Native X11 qualification
passes preview visibility/generation changes, pause/SIGSTOP/resume, paused
resize and local seek. The extended `hip_program_fixture --verify-reset` also
checks invalid-output admission, repeated renders, reset/ID behavior, distinct
allocations for retained panoramas, exhaustion at the explicit bound, and frame
lifetime after producer destruction. It reads back eight bytes per lifetime
check solely in the hardware verifier. CPU component tests cover ordered stage
execution, shared-header isolation, parent-pool lifetime without memory copying,
state reset, EOS, unconfigured startup and exception/error propagation. The
full local NVIDIA x86 build and V100 crop/overlay tests pass. Meshy's full
NVIDIA x86 build and component/tracking tests also pass with GStreamer 1.28
and FFmpeg 8. Pool-boundary qualification rejects one frame before GPU setup
and processes 12 frames with the minimum two-frame pool in both FAKE and HEVC
archive modes; the archive fully decodes. Settings regressions cover zero,
one and two frames on native and HIP builds. ARM64/Jetson validation retains
the documented missing host/sysroot and unreachable `stubby`.

### Editable desktop defaults

The shared desktop Defaults selector saves `~/.config/hstream-ui/defaults.yaml`.
AMD `LoadProgramLaunchConfig` consumes it through the same per-user loader as
NVIDIA, below game overrides. Reset to Baseline stages the bundled controls and
Save Preset clears the overlay. This changes configuration only; GPU frame
ownership and preview transfers are unchanged. `launch_config_test` covers
inheritance, game precedence, empty defaults, and invalid-file rejection.

### Rink minimap in encoded output

`hmrinkmap` now shares the NVIDIA minimap's backend-neutral projection/smoothing
and the portable GPU compositor. `output.rink-map` has identical YAML settings
on both backends, including baseline/Defaults/game precedence and the desktop
output controls. AMD attaches the frame's tracked stitched coordinates before
`hmamdcrop` hands its exclusive P010 VA surface to this element; the overlay
runs before the completion sink can retain that frame. The existing output pool
bounds memory. HIP import/completion fences the VA encoder without pixel readback.
Preview and discard outputs remain clean.

The existing AMD team/jersey inference and RTSP/WebRTC output gaps remain;
tracked-player map dots are available for AMD file encoding. The portable map
accepts team/jersey results when supplied; it never invents missing labels.
The local AMD preview minimap is still a separate port gap.

`//src/gst-plugins/gst-rink-map:rink_map_gpu_test` validates 720p/4K P010 output
on both backends, including real GPU pixel changes, timing, stale-map suppression
and EOS. `rink_map_test` covers shared configuration layering and immutable
metadata; `defaults_preset_test` covers UI save/inheritance/reset.
