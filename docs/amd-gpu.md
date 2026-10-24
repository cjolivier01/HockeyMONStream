# AMD GPU port

Status: native HIP kernels, VA/HIP video interoperability, and native MIGraphX
detector execution work on AMD. **The
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

On the installed GStreamer/Mesa stack, repeatedly creating and destroying entire
test pipelines retains DMA-BUF descriptors even with HIP processing removed.
The import/release checks on a retained surface do not leak descriptors. Repeated
application restart/seek qualification must resolve the separate media-stack
lifetime issue before claiming production playback support. RGBA/BGRA import
layouts are accepted by the adapter, but only NV12/P010 codec paths are qualified.

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
and P010. YUV callers supply color matrix/range coefficients from their negotiated
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
against OpenCV float bilinear sampling, and malformed proposal filtering. The
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

## Application work still required

| Boundary | Current implementation | Remaining AMD work |
| --- | --- | --- |
| Decode and encode | NVIDIA codecs and NVMM; standalone VA/HIP codec tests | Integrate AMD codecs and imported surfaces into the application graph |
| Allocation and batching | `NvBufSurface`, NVIDIA pools and DeepStream metadata | Portable surface ownership and synchronization integrated with GStreamer |
| Stitching integration | HIP-capable `hm-cupano` called by a DeepStream plugin | Connect HIP to portable surfaces; port high-bit conversion adapters |
| Model execution | TensorRT application; standalone native MIGraphX detector | Application integration, model preparation/cache identity, broader numerical qualification |
| Tracking | DeepStream tracker interfaces and native camera policy | Tracker integration preserving observations, resets, and seek behavior |
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
