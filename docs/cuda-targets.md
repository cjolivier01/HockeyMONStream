# CUDA targets and runtime compilation

Source and desktop package builds embed native `sm_86` (RTX 3060 Ti),
`sm_89` (RTX 4090) and `sm_120` (RTX 5090) code. Other supported GPUs JIT
compile the embedded `compute_75` PTX. Volta/V100 (`sm_70` / `compute_70`)
is excluded. The three native targets require CUDA 12.8 or newer.

This applies to direct Bazel invocations, `./bld`, `./perf` and Make targets,
including CUDA dependencies built by Bazel. `--config=blackwell` remains a
compatible alias for the same architecture list. Jetson source and package
builds instead embed `compute_87` PTX for Orin, without native CUDA code.
Desktop packages use the pinned CUDA 13.2 toolkit and the same three native
targets plus Turing PTX. Package validation checks both native and PTX
payloads. Imported SDK binaries retain their vendor-supplied kernels;
AMD/HIP kernels do not use these CUDA flags.

## Driver compilation and caching

The CUDA driver uses compatible native code when present. Otherwise it can
compile embedded PTX for a GPU with the same or a newer compute capability.
The Turing PTX covers GPUs below Ampere 8.6, such as A100 (8.0), as well
as newer architectures. Architecture-specific source branches are compiled
for that PTX baseline, so JIT does not enable newer source-level features.
PTX does not run backwards on V100. This covers our compiled kernels, not
the support matrix of external SDKs such as DeepStream or TensorRT.

The driver caches successfully compiled code on disk, normally under
`~/.nv/ComputeCache` on Linux, and reuses it across processes. The first run
may take longer. Changed code, driver upgrades, cache eviction or a disabled
cache can cause recompilation. `CUDA_CACHE_PATH` selects another writable
cache directory; `CUDA_CACHE_MAXSIZE` controls its size, and
`CUDA_CACHE_DISABLE=1` disables it. This is separate from HStream's TensorRT
engine cache. No application-managed compilation or cache is needed.

The driver must understand the PTX ISA emitted by the build toolkit.
Selecting an older compute target does **not** emit an older PTX ISA.
For example, CUDA 13.5 PTX failed on the RTX 3060 Ti with driver 580.196.07:
`the provided PTX was compiled with an unsupported toolchain`.
Adding native `sm_86` avoids JIT for these kernels on that card. A cache
cannot repair unsupported PTX; fallback GPUs need a compatible driver or
a build with a compatible toolkit/native target.

## Overrides and verification

For a machine with another GPU and an older driver, include its native
architecture explicitly. Quote the argument because semicolons are shell
separators; the toolkit must support every requested architecture:

```bash
bazelisk build --config=opt \
  '--@rules_cuda//cuda:archs=sm_80;sm_86;sm_89;sm_120;compute_75' //...
```

Inspect both executables and loaded plugins with the build toolkit's
`cuobjdump --list-elf FILE` and `cuobjdump --list-ptx FILE`. A linked binary
alone does not establish the contents of a separately loaded plugin.
Run GPU tests with `CUDA_DISABLE_PTX_JIT=1` to verify native coverage.
On a driver that supports the toolkit's PTX, `CUDA_FORCE_PTX_JIT=1` can
exercise the fallback; unset these diagnostic overrides for normal use.
