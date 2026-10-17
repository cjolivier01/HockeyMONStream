# GPU throughput and occupancy

Measure completed Program frames per second alongside GPU counters. A GPU can
remain busy while its kernels wait for memory, and a faster kernel can reduce
both execution time and occupancy. Increasing a utilization number alone is not
a throughput improvement.

`nvidia-smi` GPU utilization measures the fraction of time with GPU work active.
Nsight Systems' **SMs Active** measures activity across the SMs. **Compute Warps
in Flight** reports resident compute warps relative to the hardware limit.
Nsight Compute provides per-kernel achieved occupancy and identifies register,
shared-memory and block-size limits. These are different measurements; do not
label a 100% `nvidia-smi` reading as 100% occupancy.

## Batching and memory

The normal URI-MULTIPLE graph supplies one exact camera pair to `hmstitcher`,
which emits one stitched frame. Increasing `primary-gie.batch-size` alone does
not collect frames across incoming buffers. A larger engine can reserve more
resources without processing a larger batch. Temporal batching would require
changes to accumulation, EOS tails, seeking, frame metadata, tracker ordering
and decoder-pool retirement. It is not a safe configuration-only optimization.

Large stitched canvases already launch many more blocks than the GPU can run
simultaneously. Batching those canvases cannot remove a per-SM register limit or
increase memory bandwidth. Additional buffer pools help only when upstream work
is starved by retained buffers; each extra surface at native panorama resolution
is expensive. Keep decoder pairs lossless and do not introduce leaky queues to
raise reported FPS.

The standard workspace retains independent blend intermediates. The low-memory
profile reuses scratch and reduces pools and detector resources; see
[native-canvas memory](native-canvas-memory.md). Preserve that choice when memory
is constrained. A larger memory footprint should require a measured throughput
benefit. Kernel launch geometry itself requires no additional video surfaces or
scratch allocations.

## Measured change on gse-16a

The hm-cupano update changes the reference Laplacian kernels to 32-by-4 thread
blocks, shortens reconstruction register lifetimes, and sets a reconstruction
register budget for SM 12.0. The two-, three-, and N-camera paths receive the
same changes. It keeps the existing alpha diagnostics and performs the same
interpolation and blending, with no new GPU allocations or larger pools.

RTX 5090 (32 GiB), CUDA 13.3, DeepStream 9.1, native Blackwell builds of both
revisions, warmed FP16 detector, two-camera RGB10 input and FP16 Laplacian
stitching, 14,468-by-4,190 native panorama, start 00:09:56, duration 30 seconds:

| Measurement | Previous hm-cupano `39f3285` | Updated |
| --- | ---: | ---: |
| Program/FAKE FPS, two unprofiled runs | 90.05 / 90.60 | 93.72 / 94.77 |
| SMs Active, steady-state profile | 96.43% | 96.25% |
| Compute Warps in Flight, steady-state profile | 72.58% | 78.62% |
| Full-resolution reconstruction kernel | 981.9 us | 906.4 us |
| Full-resolution Laplacian kernel | 960.5 us | 871.4 us |
| Reconstruction registers per thread | 56 | 40 |
| Preview + 8K Program + 4K Program + stitched archives, FPS | 54.17 | 55.43 |
| Peak process memory with those archives | 14,422 MiB | 14,422 MiB |

FPS values are the final cumulative completed-output rates reported by the
pipeline. Program/FAKE improves about 4.3% across the two runs; the single archive
comparison improves about 2.3%. GPU counters are averages between the first and
last `convertHalf4ToRgba8Kernel`, excluding the first three and last one seconds.
These counters come from separate profiled Program/FAKE runs. Runs containing
unrelated GPU computations were excluded. Program/FAKE process peaks varied
between 10,350 and 10,596 MiB before and 10,350 and 10,476 MiB afterward.

This does not achieve 100% full-program occupancy. CUDA's occupancy API confirms
twelve resident 128-thread reconstruction blocks on SM 12.0 (100% theoretical
occupancy), but the observed Program/FAKE path
still averages 78.62% resident compute warps. SM activity was already near 96%.
More memory or a larger batch does not by itself remove these kernel limits.
Further work should measure kernel fusion and overlap between independent
stages; true temporal inference batching needs the lifecycle changes described
above. Optional FP16/INT8 detection and alpha blending can reduce work, but have
accuracy or image-quality tradeoffs and are separate choices.

With `--options=runtime.gpu_memory_profile=low`, the updated Program/FAKE run
completed at 69.26 FPS and peaked at 5,480 MiB. This qualification run built its
batch-one detector engine during startup; its memory peak includes that work.
The baseline/candidate comparisons above used warmed engines. Native preview with 4K Program
and stitched archives completed at 76.49 FPS and 8,689 MiB; this omits the 8K
Program encoder and is not a comparison with the three-archive row above.
Low-memory qualification used the same 32-GiB GPU with the profile forced on,
not a physically smaller GPU. The existing automatic profile and explicit
overrides remain in effect; this optimization does not grow their working sets.

With native preview and all three archives enabled, a separate profile measured
61.03% SM activity and 47.91% resident compute warps. Its window uses
`convertHalf4ToBgr10A2Kernel` with the same startup/shutdown exclusions; archives
retain the 10-bit stitched output. These numbers describe a different sink/format
configuration from Program/FAKE and must not be compared as a before/after change.
An unprofiled repeat measured 84.5% average encoder utilization (77–89% over
seconds 8–35.5). NVENC engines are separate from the SMs. Archive scheduling and
encoder throughput are additional profiling targets when those sinks are active.

All qualified runs have consecutive decoded sequences, over 30 seconds of
telemetry and completed EOS records. The archive checks found HEVC video and AAC
audio at 7680x4320, 3840x2160 and 8192x2372 (the stitched encoder limit). The first
three decoded frames of the stitched and 4K Program archives match the baseline
byte for byte.
Preview qualification uses `--ui-preview-windows=program:<XID>` and
`--ui-preview-active=program`, the desktop app's GPU-native path. The legacy
standalone renderer stalled with a CUDA copy error on this setup and was excluded.
The x86 build and all 167 hstream/dependency tests pass. The Jetson ARM64
cross-build passes; Jetson runtime/performance and native ARM64/SBSA remain
unqualified because no reachable host was available.

## Reproducing measurements

Use an isolated copy of a completed game, preserve calibration timestamps, link
the original camera videos, and direct archives and telemetry to a private output
directory. Warm the detector engine separately. Confirm that calibration and
engine construction did not run inside the measurement. Disable normal-speed
playback when measuring maximum throughput.

On an RTX 5090, build native Blackwell code with `./perf` or
`bazelisk build --config=opt --cpu=k8 --config=blackwell //...`. Record the CUDA
architecture flags, detector engine identity, video dimensions, blend mode,
precision, sinks, GPU clocks, memory use and any competing processes. Compare
both revisions with the same build flags and engine.

Capture CUDA kernels and GPU metrics separately from ordinary throughput runs:

```sh
nsys profile --trace=cuda,nvtx --sample=none --cpuctxsw=none \
  --gpu-metrics-devices=0 --gpu-metrics-frequency=1000 \
  --output=/path/to/results/profile /path/to/hstream-cli \
  -g gse-16a --start-time=00:09:56 -t=30 \
  --enable-sources=URI-MULTIPLE --enable-sinks=FAKE \
  --options=pipeline.hmaudio.enable=1
nsys export --type=sqlite --output=/path/to/results/profile.sqlite \
  /path/to/results/profile.nsys-rep
nsys stats --report=cuda_gpu_kern_sum,cuda_api_sum \
  /path/to/results/profile.nsys-rep
```

GPU performance counters may require elevated profiling privileges. Keep the
application's user and cache environment when running a privileged profiler.
Measure a steady-state window that excludes startup and shutdown. GPU metrics
are device-wide: discard runs with unrelated GPU work, even if the pipeline's
own frame timing looks plausible.

An exit code of zero is insufficient. Verify consecutive decoded sequences,
the requested video duration and a completed telemetry run. Qualify native
preview and encoding separately from Program/FAKE, and repeat with the
low-memory profile. Startup, chapter transitions, seeks and shutdown must still
release GPU buffers correctly.
