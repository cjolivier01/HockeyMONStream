#pragma once

#include <gst/gst.h>
#include "hstream/src/libs/gpu/Runtime.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::gpu {

// A borrowed VA-API frame imported into HIP. No pixel mapping or host copy.
// Import waits for VA's producer; Complete waits for the caller's HIP stream
// before a VA consumer may use the frame. The buffer is retained until destruction.
// The stream must belong to the current HIP device and outlive this object.
// All work using the pointers must use this stream and precede Complete().
// Destruction also drains the stream on error/early-return paths.
class VaHipFrame {
 public:
  enum class Access { kRead, kWrite };
  enum class Format { kRgba8, kBgra8, kNv12, kP010 };
  struct Plane {
    void* data;
    size_t pitch;
    size_t row_bytes;
    uint32_t height;
  };

  // Only linear VA surfaces are accepted. Negotiate memory:DMABuf and a linear
  // DRM format through vapostproc for decoder surfaces with a tiled layout.
  // kWrite requires exclusive buffer/memory ownership before taking our ref.
  static absl::StatusOr<std::unique_ptr<VaHipFrame>> Import(GstBuffer* buffer, hipStream_t stream, Access access);
  ~VaHipFrame();
  VaHipFrame(const VaHipFrame&) = delete;
  VaHipFrame& operator=(const VaHipFrame&) = delete;

  Format format() const {
    return format_;
  }
  uint32_t width() const {
    return width_;
  }
  uint32_t height() const {
    return height_;
  }
  unsigned plane_count() const {
    return plane_count_;
  }
  const Plane& plane(unsigned index) const {
    return planes_.at(index);
  }
  absl::Status Complete();

 private:
  VaHipFrame() = default;
  struct Object {
    int fd{-1};
    hipExternalMemory_t memory{nullptr};
    void* data{nullptr};
  };
  GstBuffer* buffer_{nullptr};
  hipStream_t stream_{nullptr};
  int device_{-1};
  bool complete_{false};
  Format format_{};
  uint32_t width_{0}, height_{0};
  unsigned plane_count_{0};
  std::array<Plane, 4> planes_{};
  std::array<Object, 4> objects_{};
};

} // namespace hm::gpu
