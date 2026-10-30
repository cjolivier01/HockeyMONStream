#pragma once

#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/common/StitchingFramePairMeta.h"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace hm::preview {
// Fixed-size copies only: no GstBuffer/video memory retention. Physical chapter
// PTS must never be replaced with a local pipeline PTS or an estimated UI time.
struct CaptureMetadata {
  std::optional<StitchingFramePair> pair;
  std::optional<preview_overlay::PlayCropperTransform> crop;
};
CaptureMetadata ReadCaptureMetadata(GstBuffer* buffer);

struct CaptureContext {
  std::string interface;
  std::string run;
  int stage{0};
  uint64_t topology{0}, operation{0}, sequence{0};
  std::string channel;
  GstClockTime source_pts{GST_CLOCK_TIME_NONE}, presentation_pts{GST_CLOCK_TIME_NONE};
  uint64_t renderer_generation{0};
  std::string path;
};
std::string CaptureReceipt(
    const CaptureContext& context,
    const CaptureMetadata& metadata,
    const std::string& request = "",
    unsigned occurrence = 0);

// Explicit launch-only diagnostic plan, maximum 8 requests x 3 receipts. It
// captures metadata only; the browser must independently capture a verified
// displayed frame with the exact run/topology/operation/channel/sequence.
class CapturePlan {
 public:
  explicit CapturePlan(const char* json);
  std::optional<std::string> Capture(const CaptureContext& context, const CaptureMetadata& metadata);
  bool empty() const {
    return requests_.empty();
  }
  bool pending() const {
    return remaining_.load(std::memory_order_relaxed) != 0;
  }

 private:
  struct Request {
    std::string id, channel;
    GQuark left_uri{0}, right_uri{0};
    GstClockTime left_pts{GST_CLOCK_TIME_NONE}, right_pts{GST_CLOCK_TIME_NONE};
    unsigned count{0}, captured{0};
    std::vector<std::string> identities;
  };
  std::vector<Request> requests_;
  std::mutex mutex_;
  std::atomic<unsigned> remaining_{0};
};
// One process-wide budget survives WebPreviewSession reconstruction. The
// environment is read once; omitted means disabled. Invalid explicit plans fail
// prelaunch validation rather than silently collecting a different frame.
CapturePlan& ProcessCapturePlan();
} // namespace hm::preview
