#include "hstream/src/libs/detection/DetectionPostprocess.h"

#include <algorithm>
#include <cmath>

namespace hm::detection {
absl::StatusOr<Letterbox> MakeLetterbox(
    int source_width,
    int source_height,
    int network_width,
    int network_height,
    bool maintain_aspect,
    bool symmetric_padding,
    float scale) {
  if (source_width <= 0 || source_height <= 0 || network_width <= 0 || network_height <= 0 || !std::isfinite(scale) ||
      scale <= 0)
    return absl::InvalidArgumentError("Invalid detector source, input geometry or scale");
  Letterbox box{network_width, network_height, network_width, network_height, 0, 0, scale};
  if (maintain_aspect) {
    const double ratio = std::min(
        static_cast<double>(network_width) / source_width, static_cast<double>(network_height) / source_height);
    box.resized_width = std::max(1, std::min(network_width, static_cast<int>(source_width * ratio)));
    box.resized_height = std::max(1, std::min(network_height, static_cast<int>(source_height * ratio)));
    if (symmetric_padding) {
      box.left = (network_width - box.resized_width) / 2;
      box.top = (network_height - box.resized_height) / 2;
    }
  }
  return box;
}

absl::StatusOr<std::vector<Proposal>> SuppressProposals(
    std::vector<Proposal> proposals,
    float iou_threshold,
    int maximum_detections,
    const Letterbox& box,
    int source_width,
    int source_height) {
  if (!std::isfinite(iou_threshold) || iou_threshold < 0 || iou_threshold > 1 || maximum_detections <= 0 ||
      source_width <= 0 || source_height <= 0 || box.width <= 0 || box.height <= 0 || box.resized_width <= 0 ||
      box.resized_height <= 0 || box.left < 0 || box.top < 0 || box.left > box.width - box.resized_width ||
      box.top > box.height - box.resized_height)
    return absl::InvalidArgumentError("Invalid detector suppression or letterbox geometry");
  for (const auto& p : proposals) {
    if (!std::isfinite(p.left) || !std::isfinite(p.top) || !std::isfinite(p.right) || !std::isfinite(p.bottom) ||
        !std::isfinite(p.confidence) || p.confidence < 0 || p.confidence > 1 || p.class_id < 0 || p.left < 0 ||
        p.top < 0 || p.right > box.width || p.bottom > box.height || p.right <= p.left || p.bottom <= p.top)
      return absl::InvalidArgumentError("Invalid filtered detector proposal");
  }
  std::sort(proposals.begin(), proposals.end(), [](const Proposal& a, const Proposal& b) {
    return a.confidence != b.confidence ? a.confidence > b.confidence : a.source_index < b.source_index;
  });
  std::vector<Proposal> selected, mapped;
  for (const auto& p : proposals) {
    bool suppressed = false;
    for (const auto& q : selected) {
      if (p.class_id != q.class_id)
        continue;
      const double intersection = std::max(0.f, std::min(p.right, q.right) - std::max(p.left, q.left)) *
          static_cast<double>(std::max(0.f, std::min(p.bottom, q.bottom) - std::max(p.top, q.top)));
      const double area = (p.right - p.left) * static_cast<double>(p.bottom - p.top) +
          (q.right - q.left) * static_cast<double>(q.bottom - q.top) - intersection;
      if (intersection / area > iou_threshold) {
        suppressed = true;
        break;
      }
    }
    if (suppressed)
      continue;
    Proposal source = p;
    const double sx = static_cast<double>(source_width) / box.resized_width;
    const double sy = static_cast<double>(source_height) / box.resized_height;
    source.left = std::clamp((p.left - box.left) * sx, 0., static_cast<double>(source_width));
    source.right = std::clamp((p.right - box.left) * sx, 0., static_cast<double>(source_width));
    source.top = std::clamp((p.top - box.top) * sy, 0., static_cast<double>(source_height));
    source.bottom = std::clamp((p.bottom - box.top) * sy, 0., static_cast<double>(source_height));
    // A prediction wholly in padding must not consume the output limit.
    if (source.right <= source.left || source.bottom <= source.top)
      continue;
    selected.push_back(p);
    mapped.push_back(source);
    if (mapped.size() == static_cast<size_t>(maximum_detections))
      break;
  }
  return mapped;
}
} // namespace hm::detection
