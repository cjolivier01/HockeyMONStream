#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "hstream/src/libs/stitching/GameConfig.h"

namespace hm::stitching {

struct VideoLensMetadata {
  StitchCameraSelection camera;
  std::string model;
  std::string stabilization;
  std::string mode;
  size_t width{0};
  size_t height{0};
  // Camera/settings remain available for mismatch warnings even when its warp
  // cannot be represented by the shared Hugin model.
  std::string unavailable_reason;
};

// Bounded MP4 header/trailer and first DJI metadata sample reads only.
// Never decodes video or scans mdat.
// Results are cached by file identity, size and modification/change timestamps.
absl::StatusOr<VideoLensMetadata> ReadVideoLensMetadata(const std::filesystem::path& video);
// Exposed for bounded malformed-input and model-conversion tests.
absl::StatusOr<VideoLensMetadata> ParseVideoLensMetadata(const std::vector<unsigned char>& gpmf);

absl::StatusOr<VideoLensMetadata> ParseInsta360LensMetadata(const std::vector<unsigned char>& data);
absl::StatusOr<VideoLensMetadata> ParseDjiLensMetadata(
    const std::vector<unsigned char>& data,
    size_t width,
    size_t height);

struct AutoInputLensResult {
  StitchCameraSelection camera;
  std::string message;
  bool detected{false};
  bool mismatch{false};
};

// Inspects every selected chapter. Only agrees to a shared prescription when
// every input has compatible metadata. Missing/unsupported/different metadata
// leaves automatic fitting in use, with a visible reason for the operator.
AutoInputLensResult ResolveAutoInputLens(
    const std::vector<std::filesystem::path>& videos,
    const StitchCameraSelection& fallback);

} // namespace hm::stitching
