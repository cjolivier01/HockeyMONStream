#pragma once

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

#include <functional>
#include <memory>

namespace hm::draw_display {

enum class GpuGlStatus { kOk, kInvalidArgument, kUnavailable, kGpuError, kGlError, kCallbackFailed };
struct GpuGlResult {
  GpuGlStatus status{GpuGlStatus::kOk};
  cudaError_t gpu_error{cudaSuccess};
  unsigned egl_error{0}, gl_error{0};
};
struct GpuGlCounters {
  uint64_t frames{0}, copied_bytes{0}, texture_bytes{0}, texture_allocations{0};
};

// A bounded, private compatibility-GL framebuffer on the current CUDA/HIP
// device. No X window, host video mapping/readback, or software fallback. Two
// RGBA textures handle opposite GL/video row orientation entirely on the GPU.
// Paint borrows only an independently owned opaque RGBA preview subimage;
// letterbox bars are outside its image bounds. Each pass makes two D2D copies.
// One device/stream/geometry per instance. Caller completes its stream before
// destruction (also after errors). Serialized calls may migrate threads; a
// concurrent call is rejected. Context binding is restored on every call.
class GpuGlSurface {
 public:
  // Resource ceiling, not a resolution default: at most 32 MiB per texture.
  static constexpr uint64_t kMaximumPixels = 8U * 1024U * 1024U;
  GpuGlSurface();
  ~GpuGlSurface();
  GpuGlSurface(const GpuGlSurface&) = delete;
  GpuGlSurface& operator=(const GpuGlSurface&) = delete;

  // Allocate and check GL interop before the caller starts any preview writes.
  // Does not access image pixels. Unavailable interop retires this owner.
  GpuGlResult Prepare(const analytics::ImageView& owned_video, cudaStream_t stream);

  // Callback runs with identity matrices and the complete video viewport. It
  // must not bind another framebuffer or retain GL objects/pointers. The RGB
  // result is preserved; alpha is made opaque before returning to the encoder.
  GpuGlResult Paint(const analytics::ImageView& owned_video, cudaStream_t stream, const std::function<bool()>& draw);
  const GpuGlCounters& counters() const;

 private:
  struct State;
  std::unique_ptr<State> state_;
};

} // namespace hm::draw_display
