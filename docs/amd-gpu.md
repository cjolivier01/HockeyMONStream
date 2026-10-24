# AMD GPU port

Status: native HIP kernels, VA/HIP video interoperability, and native MIGraphX
detector execution work on AMD. A standalone raw-camera Program qualifier now
composes stitching, detection, rink filtering, tracking, crop and hardware encode. **The
`hstream-cli` and desktop video pipelines still require NVIDIA.** This is the
compute/video/inference foundation of the port, not an AMD release of the application.

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
the future playlist owner must supply chapter offsets and pair the cameras.
Cancellation is permanent and checked between bounded waits. GStreamer/driver
calls themselves are not interruptible. Errors retire the reader.

Repeated chapter changes exposed an upstream GStreamer 1.24.2 leak:
`gst_va_dmabuf_get_modifier_for_format` exports a temporary VA surface but never
closes its DMA-BUF handles. On this host that leaked 13 descriptors per restart,
without HIP involvement. The private build below fixes the probe and failed
exports, leaving the installed library, plugins and core unchanged. It builds
only `libgstva-1.0` against the installed 1.24.2 development packages. The source
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
driver call is not promised or tested. The composed raw Program qualifier now uses
the reusable reader. Full application source/audio lifecycle and desktop
integration remain unqualified. RGBA/BGRA import layouts are accepted by the
adapter, but only NV12/P010 codec paths are qualified.

## Native AMD inference

`src/libs/gpu/migraphx/MigraphxSession.*` compiles static ONNX shapes with
MIGraphX's native GPU target. It supports FP32 or FP16 operations with contiguous
FP32 bindings. It disables automatic copies, allocates bindings once, and uses
one explicit caller-owned HIP stream. Destruction drains that stream before
freeing model storage. All input shapes and the binding byte bound are explicit;
unsupported tensor contracts fail preparation. Compiled-model caching is still
pending, so preparation currently recompiles the model at startup.

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
Bazel tests need `--test_env=LD_LIBRARY_PATH` to retain that selection; the Make
target above includes it. The two hardware tests initialize MIGraphX, VA and EGL
in opposite orders and render a synthetic OpenGL framebuffer on the selected
HIP device. Their only pixel readback is four test bytes. The real 120-frame
video fixture and the multi-input FP32/FP16 session tests also pass with this
private core. System libva, Mesa and ROCm libraries are never modified. Packaging
and CLI/UI runtime discovery still need to incorporate this qualified runtime.

## Application work still required

| Boundary | Current implementation | Remaining AMD work |
| --- | --- | --- |
| Decode and encode | NVIDIA codecs and NVMM; standalone VA/HIP codec tests | Integrate AMD codecs and imported surfaces into the application graph |
| Allocation and batching | `NvBufSurface`, NVIDIA pools and DeepStream metadata | Portable surface ownership and synchronization integrated with GStreamer |
| Stitching integration | HIP-capable `hm-cupano` called by a DeepStream plugin | Connect HIP to portable surfaces; port high-bit conversion adapters |
| Model execution | TensorRT application; standalone native MIGraphX detector | Application integration, model preparation/cache identity, broader numerical qualification |
| Tracking | Native geometric tracker and shared rink/camera policy qualified independently | Application integration preserving observations, resets and seek behavior |
| Crop and preview | CUDA transforms and NVIDIA graphics interop | Portable crop/surface adapters and qualified AMD graphics interop |
| Desktop, jobs and publication | NVIDIA runner and media paths | Playback, seeking, audio, archive and highlight export on the AMD backend |

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
that RGB VPP negotiation bug and its extra conversion pass. No private
GStreamer build or misleading caps are needed. The canonical render-node path
is also required: VA factories compare their path with the context verbatim,
so passing a PCI symlink alias can trigger an error while starting elements.

This remains component qualification. Raw-camera pairing/stitching, full Program
configuration/startup, model caching, audio, seek/reset, native desktop controls
and installed AMD runtime discovery are still pending.


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

The current CLI/UI acceptance workflow remains incomplete. Calibration creation,
application configuration, model caching, source/audio lifecycle and native desktop
preview must be integrated and qualified before AMD application support is claimed.

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
choices remain explicit qualification arguments. CLI/UI configuration loading
and runtime ownership are still pending.

## Reusable Program processor

`program/AmdProgramProcessor` now owns the validated saved panorama, GPU
workspaces, MIGraphX detector, rink pruning, object association and native camera
policy. The raw-camera qualifier uses this owner. `Process` consumes a paired
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
reusable owner is not yet connected to CLI/UI playback.

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
cancellation is permanent. The raw Program qualifier now uses this owner, with
one file per camera; application source selection and stereo/audio coordination
remain pending.

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
