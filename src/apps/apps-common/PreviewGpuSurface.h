#pragma once

#include <cuda_runtime_api.h>
#include <gst/gst.h>

#include <cstddef>
#include <cstdint>
#include <functional>

namespace hm::preview {

struct SurfaceRectangle {
  std::uint32_t left, top, width, height;
};

// Called after the source is scaled into an independently owned RGBA preview
// surface. Metadata stays on selected_frame/source_frame(selected_frame).
// Paint only this output, enqueue work on stream, and return before retaining
// either frame or view. The adapter completes that stream before publication.
struct OwnedSurfaceView {
  void* rgba;
  std::size_t pitch;
  std::uint32_t width, height;
  SurfaceRectangle video;
  cudaStream_t stream;
};
using SurfacePainter = std::function<bool(GstBuffer* selected_frame, const OwnedSurfaceView& output)>;

struct GpuSurfaceConfig {
  // Every value is required from the caller's resolved configuration. Zero
  // dimensions/rate/pool are invalid; this API defines no shipped defaults.
  std::uint32_t width{0}, height{0};
  std::uint32_t rate_numerator{0}, rate_denominator{0};
  std::uint32_t pool_frames{0};
  std::uint32_t gpu_id{0};
};

// One stream, fixed output geometry/cadence, bounded GPU surface pool. Exhausted
// slots drop observational frames without waiting on the processing/archive
// producer. Raw pixels never enter host memory; only NvBufSurface descriptors
// are inspected on the CPU. Currently CUDA-device surfaces only.
GstElement* gpu_surface_create(const char* name, const GpuSurfaceConfig& config, SurfacePainter painter);

} // namespace hm::preview
