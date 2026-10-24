#pragma once

#include <vector>

#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/DetectionTypes.h"

namespace hm::detection {

absl::StatusOr<Letterbox> MakeLetterbox(
    int source_width,
    int source_height,
    int network_width,
    int network_height,
    bool maintain_aspect,
    bool symmetric_padding,
    float scale);

// Class-aware greedy NMS on model-space boxes, then undo the exact letterbox
// and clip to source pixels. Score ties use the original tensor row index, so
// parallel GPU compaction cannot change tracking inputs between runs.
absl::StatusOr<std::vector<Proposal>> SuppressProposals(
    std::vector<Proposal> proposals,
    float iou_threshold,
    int maximum_detections,
    const Letterbox& box,
    int source_width,
    int source_height);

} // namespace hm::detection
