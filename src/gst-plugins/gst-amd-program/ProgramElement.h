#pragma once

#include <gst/gst.h>
#include <memory>
#include "absl/status/statusor.h"

namespace hm::program::gst {
// One processor belongs to one element. Configure before READY; streaming and
// resets are serialized by the sink pad. No borrowed configuration survives.
class Component {
 public:
  virtual ~Component() = default;
  // Input is exclusively owned by the caller until return. Return that same
  // buffer for in-place work, or a newly owned replacement. Never consume input.
  virtual absl::StatusOr<GstBuffer*> Process(GstBuffer* input) = 0;
  virtual absl::Status Reset() = 0;
  // Called outside the streaming lock on FLUSH_START and teardown. Implementors
  // that wait for resources must unblock Process; Reset re-enables admission.
  virtual void Interrupt() noexcept {}
  // Optional newly owned caps after a format-changing operation.
  virtual GstCaps* OutputCaps() const {
    return nullptr;
  }
};
bool RegisterProgramPlugin();
// Takes ownership. Creates only a writable buffer header when a sink still
// retains the original; refs the exact memories and pins the original pool
// buffer until downstream releases the new header. Never maps/copies pixels.
absl::StatusOr<GstBuffer*> WritableHeader(GstBuffer* input);
absl::StatusOr<GstElement*> CreateElement(
    const char* factory,
    const char* name,
    std::unique_ptr<Component> processor,
    GstCaps* output_caps = nullptr);
} // namespace hm::program::gst
