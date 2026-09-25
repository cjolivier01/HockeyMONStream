#pragma once

#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/Config.h"
#include "hstream/src/libs/player_analytics/ModelContract.h"
#include "hstream/src/libs/player_analytics/runtime/SemanticGpu.h"

namespace hm::player_analytics {

enum class JerseyCropReason { kReady, kInvalidGeometry, kInsufficientPose, kOutsideImage, kTooLarge };
struct JerseyCropSelection {
  JerseyCrop crop;
  JerseyCropReason reason{JerseyCropReason::kInvalidGeometry};
  explicit operator bool() const noexcept {
    return reason == JerseyCropReason::kReady;
  }
};

// Metadata-only, no pixels/allocations. Bbox mode selects x20–80%, y25–95%
// of the tracking box. Pose mode requires finite shoulders/hips5,6,11,12 with
// confidence>=.4 and selects their bounds plus5% per side. It never falls back.
// Map each axis independently to surface coordinates, floor left/top and ceil
// right/bottom. Partially outside crops retain black padding; fully outside or
// degenerate/over8192 crops produce a nonfatal skip reason. The caller owns pose
// freshness/identity checks before supplying it.
JerseyCropSelection MakeJerseyCrop(
    const ImageView& image,
    const Box& box,
    float metadata_width,
    float metadata_height,
    JerseyRoiMode mode,
    const Pose* pose = nullptr) noexcept;

absl::StatusOr<JerseyVocabulary> MakeJerseyVocabulary(const ModelManifest& manifest);

} // namespace hm::player_analytics
