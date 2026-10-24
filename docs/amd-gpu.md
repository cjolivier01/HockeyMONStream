# AMD GPU port

Status: native HIP builds of the shared compute kernels work on AMD. **The
`hstream-cli` and desktop video pipelines still require NVIDIA.** This is the
compute portion of the port, not an AMD release of the application.

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

# Equivalent test command; also works without --config=rocm on NVIDIA.
bazelisk test --config=opt --config=rocm --local_test_jobs=1 --test_output=errors \
  //src/libs/gpu:portable_gpu_tests
```

The suite explicitly selects GPU tests, including HStream's overlay, highlights,
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

## Application work still required

| Boundary | Current implementation | Remaining AMD work |
| --- | --- | --- |
| Decode and encode | NVIDIA codecs and NVMM | AMD codecs and GPU surface import/export, with timestamp and EOS parity |
| Allocation and batching | `NvBufSurface`, NVIDIA pools and DeepStream metadata | Portable surface ownership and synchronization integrated with GStreamer |
| Stitching integration | HIP-capable `hm-cupano` called by a DeepStream plugin | Connect HIP to portable surfaces; port high-bit conversion adapters |
| Model execution | TensorRT and NVIDIA inference plugins | AMD inference, model preparation/cache identity, numerical qualification |
| Tracking | DeepStream tracker interfaces and native camera policy | Tracker integration preserving observations, resets, and seek behavior |
| Crop and preview | CUDA transforms and NVIDIA graphics interop | Portable crop/surface adapters and qualified AMD graphics interop |
| Desktop, jobs and publication | NVIDIA runner and media paths | Playback, seeking, audio, archive and highlight export on the AMD backend |

Do not pass `--config=rocm` to `//...` as an application build: many targets
still directly depend on NVIDIA libraries. Compute tests do not establish
end-to-end AMD playback or export support.

## Hardware evidence

On the RX 9060 XT (`gfx1200`, ROCm 6.4.1), the twelve-test portable suite passes.
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
