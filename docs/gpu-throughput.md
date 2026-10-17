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

## Kernel changes on gse-16a

The hm-cupano update changes the reference Laplacian kernels to 32-by-4 thread
blocks, shortens reconstruction register lifetimes, and sets a reconstruction
register budget for SM 12.0. The two-, three-, and N-camera paths receive the
same changes. It keeps the existing alpha diagnostics and performs the same
interpolation and blending, with no new GPU allocations or larger pools.

RTX 5090 (32 GiB), CUDA 13.3, DeepStream 9.1, native Blackwell builds of both
revisions, warmed FP16 detector, two-camera RGB10 input and FP16 Laplacian
stitching, 14,468-by-4,190 native panorama, start 00:09:56, duration 30 seconds:

| Measurement | Previous hm-cupano `39f3285` | Updated kernels |
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

## Borrowed panorama output

High-bit hmstitcher also requests a borrowed view of the full-canvas blend
workspace, in either standard or compact mode. Conversion and one-pass rink-mask
preparation consume that view on the same CUDA stream before the next frame.
Hard seams and minimized blending keep cached owned output. The public hm-cupano
API retains owned output by default for standard-workspace callers, and an
explicitly supplied canvas always wins. See [native-canvas memory](native-canvas-memory.md)
for the ownership contract.

This removes one 484,967,360-byte FP16 output allocation (462.5 MiB at the tested
canvas size), its per-frame clear and the full-panorama device copy. It changes
neither pixels nor precision and requires no extra in-flight frames or GPU memory.
A fresh comparison against the kernel-only revision used the same build flags,
warmed detector, recording interval and sinks:

| Measurement | Kernel-only revision | With borrowed output |
| --- | ---: | ---: |
| Program/FAKE FPS, two unprofiled runs | 95.24 / 94.71 | 103.02 / 102.35 |
| SMs Active, steady-state Program profile | 96.25% | 95.80% |
| Compute Warps in Flight, same profile | 78.60% | 79.00% |
| Full-panorama copy per frame | 626.2 us | eliminated |
| Full-panorama clears per frame | 3 | 2 |
| All three archives plus native preview, FPS | 53.49 | 55.65 |
| Peak process memory with those archives | 14,422 MiB | 14,080 MiB |

Program throughput improves another 8.1% in this comparison. The removed clear
averaged 305.3 us. Archive repeats remained around 53–56 FPS, so they do not
establish a consistent additional archive speedup. Other borrowed-output archive runs peaked between 13,958 and 14,324 MiB;
process peaks include other pools and need not fall by exactly the removed
allocation. Stream and archive-queue trials did not
show a useful gain and are not included in the change. NVENC and its scheduling
remain limits for the all-archive workload; larger inference batches do not
remove them.

The updated all-archive profile measured 57.35% SM activity and 45.29% resident
compute warps, using the same conversion-window exclusions. Removing a memory
copy reduces these whole-program counters even though Program throughput rises;
keeping SMs artificially busy would not improve archive delivery.

With the low-memory profile forced on, the final Program/FAKE run completed at
69.31 FPS and 5,474 MiB peak process memory. Native preview plus 4K Program and
stitched archives completed at 76.23 FPS and 8,689 MiB. The staged RGB10 input
fallback also completed. These runs retained consecutive decoded sequences and
completed EOS; the automatic profile, pool limits and engine selection are
unchanged. Qualification still uses the 32-GiB GPU rather than a physically
small-memory board.

Nine decoded stitched-archive frames (three each at 0, 10 and 20 seconds) match
the kernel-only baseline byte for byte. All 48,576 detection records over the
common 1,819 frames also match. The first three frames of both Program archives
match; later Program samples do not. Native tracker scores and camera decisions
vary across repeated runs of the identical binary too, so later Program archive
identity is not established. This distinction is retained in the qualification
rather than treating encoded Program hashes as a deterministic stitching test.
The 2/3/N-camera output ownership tests pass Compute Sanitizer memcheck with zero
errors, in addition to all 167 tests and the x86/Jetson builds. An injected
first-frame allocation failure after queued remaps verifies that `GenerateOutput`
waits for the stream before returning borrowed buffers to its caller. This
CUDA-device-surface fixture runs on x86; native Jetson EGL failure injection
remains unqualified.

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
