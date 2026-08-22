<!--
SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
SPDX-License-Identifier: Apache-2.0
-->

# Gst-dsxvideoconvert

`dsxvideoconvert` is a source-available, differently named GStreamer video
converter for DeepStream. It implements the documented `nvvideoconvert`
GStreamer contract without replacing or shadowing the NVIDIA plugin:

- element: `dsxvideoconvert`
- plugin: `dsxvideoconvert`
- library: `libgstdsxvideoconvert.so`

The implementation is a clean-room `GstBaseTransform` wrapper built from the
public DeepStream headers and documented APIs. It uses
`NvBufSurfTransform` for conversion, scaling, crop, interpolation, and
orientation, and `GstNvDsBufferPool` for NVMM output. It does not contain
copied NVIDIA plugin implementation code.

This is not a source replacement for the low-level conversion runtime. The
plugin still dynamically links to the binary-only `libnvbufsurface.so`,
`libnvbufsurftransform.so`, and DeepStream buffer-pool/metadata libraries.

## Features

- RAW to RAW, RAW to NVMM, NVMM to RAW, and NVMM to NVMM operation
- DeepStream batched `NvBufSurface` buffers
- documented dGPU and Jetson caps, including 10-bit, 12-bit, and 16-bit paths
- source and destination crop, scaling, all flip/rotation modes, and every
  documented interpolation method
- GPU, memory-type, output-pool, contiguous-allocation, and Jetson
  block-linear controls
- DeepStream batch metadata propagation and OSD coordinate scaling
- same documented property names, defaults, ranges, enum values, and mutable
  states as `nvvideoconvert`
- padded and custom-stride RAW buffers described by `GstVideoMeta`
- concatenated RAW output for multi-frame NVMM batches

The plugin deliberately does not export the original library's unnamespaced
helper symbols. Those functions have no public DeepStream header, and defining
them again would make the original and replacement unsafe to load in one
process. The GStreamer-facing API is the compatibility boundary.

## Bazel build

Hstream builds the plugin with Bazel alongside its other owned GStreamer
plugins:

```bash
bazelisk build --config=opt \
  //src/gst-plugins/gst-dsxvideoconvert:libgstdsxvideoconvert.so
# Or build every Hstream-owned plugin:
make hstream-gst-plugins
```

The output is
`bazel-bin/src/gst-plugins/gst-dsxvideoconvert/libgstdsxvideoconvert.so`.
Hstream's Debian-package flow stages it under Hstream's private
`lib/gst-plugins` directory; it does not replace NVIDIA's installed plugin.

By default `@deepstream` resolves the unversioned
`/opt/nvidia/deepstream/deepstream` link. To compile against a specific SDK
installation as DeepStream versions evolve, set the repository override:

```bash
bazelisk build --repo_env=DEEPSTREAM_ROOT=/opt/nvidia/deepstream/deepstream-9.1 \
  //src/gst-plugins/gst-dsxvideoconvert:libgstdsxvideoconvert.so
```

For an uninstalled build:

```bash
export DEEPSTREAM_ROOT=/opt/nvidia/deepstream/deepstream
export GST_PLUGIN_PATH="$PWD/bazel-bin/src/gst-plugins/gst-dsxvideoconvert:$DEEPSTREAM_ROOT/lib/gst-plugins"
export LD_LIBRARY_PATH="$DEEPSTREAM_ROOT/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
gst-inspect-1.0 dsxvideoconvert
```

Example:

```bash
gst-launch-1.0 videotestsrc num-buffers=30 ! \
  'video/x-raw,format=RGBA,width=1920,height=1080' ! \
  dsxvideoconvert interpolation-method=1 ! \
  'video/x-raw(memory:NVMM),format=NV12,width=1280,height=720' ! fakesink
```

## Validation

The opt-in parity suite requires the NVIDIA `nvvideoconvert` from the selected
`@deepstream` repository as a black-box oracle and runs each candidate in a
fresh process. The tests are tagged `manual` and `requires-gpu` so ordinary
CPU-only Hstream test runs do not select them:

```bash
bazelisk test --config=opt --test_output=errors \
  //src/gst-plugins/gst-dsxvideoconvert:parity_tests
```

It checks pad and negotiated caps, properties, the exact namespaced ELF ABI,
dynamic dependencies, RAW and NVMM paths, the documented format set, odd
dimensions in every RAW format, crop backgrounds across YUV formats, flips,
interpolation, metadata orientation, nonzero batched content, custom RAW
plane mappings and strides, signed caps-only batch sizing, output-pool depth,
allocation/layout controls, malformed NVMM descriptors, undersized-stride
rejection, stale layout-meta filtering, and unsupported BGRA64 conversion.
Probe processes have explicit timeouts.

The benchmark suite uses direct RAW and NVMM generators and GStreamer's
per-element latency tracer, so source generation is outside the converter
measurement. A dedicated appsrc harness measures two-surface batched NVMM
conversion and subtracts an identity baseline. The suite balances both run
orders, warms each path, covers RAW-to-RAW, RAW-to-NVMM, NVMM-to-NVMM,
NVMM-to-RAW, crop/flip, and batched workloads, uses median trial results, and
fails if the replacement exceeds the configured slowdown ceiling:

```bash
bazelisk run --config=opt \
  //src/gst-plugins/gst-dsxvideoconvert:dsxvideoconvert_benchmark
bazelisk run --config=opt \
  //src/gst-plugins/gst-dsxvideoconvert:dsxvideoconvert_benchmark -- \
  --max-slowdown 1.20 --frames 600 --trials 3
```

Benchmark results are intentionally not checked into the repository.

## References

- [DeepStream Gst-nvvideoconvert documentation](https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_plugin_gst-nvvideoconvert.html)
- `NvBufSurfTransform` and `NvBufSurface` public headers from the selected
  `@deepstream` SDK repository
