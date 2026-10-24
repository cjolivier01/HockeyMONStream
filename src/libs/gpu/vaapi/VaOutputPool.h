#pragma once

#include <gst/gst.h>
#include <gst/va/va_fwd.h>
#include <gst/video/video-info.h>
#include <filesystem>
#include <memory>

#include "absl/status/statusor.h"

namespace hm::gpu {
// Bounded linear RGBA/NV12/P010 DMA-BUF output, allocated by VA on the selected HIP
// device. Borrow it with VaHipFrame(kWrite), drain HIP, then pass the original
// buffer to VA encode or GL upload. No host mapping or pixel copies.
class VaOutputPool {
 public:
  static absl::StatusOr<std::unique_ptr<VaOutputPool>> Create(
      int hip_device,
      const GstVideoInfo& info,
      unsigned maximum_buffers);
  ~VaOutputPool();
  VaOutputPool(const VaOutputPool&) = delete;
  VaOutputPool& operator=(const VaOutputPool&) = delete;

  // Transfer-full result. Nonblocking: ResourceExhausted means a downstream
  // consumer still owns the bounded pool. The pipeline controls waiting and
  // cancellation rather than becoming stuck inside buffer acquisition.
  absl::StatusOr<GstBuffer*> Acquire();
  // Borrowed until destruction. Ref them when retaining beyond this object.
  GstCaps* caps() const {
    return caps_;
  }
  GstVaDisplay* display() const {
    return display_;
  }
  const std::filesystem::path& render_node() const {
    return render_node_;
  }
  void SetDisplayContext(GstElement* element) const;

 private:
  VaOutputPool() = default;
  GstVaDisplay* display_{};
  GstCaps* caps_{};
  GstBufferPool* pool_{};
  std::filesystem::path render_node_;
};
} // namespace hm::gpu
