#pragma once

#include <array>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::stitching {

// Source-image optics, independent of the panorama's output projection.
// Offsets use fractions of the full source width/height, so a lens calibration
// survives bounded previews and changes in decoding resolution.
struct StitchInputLens {
  std::string projection{"rectilinear"};
  std::string optimization{"auto"};
  std::array<double, 3> distortion{0, 0, 0}; // Hugin a, b, c.
  std::array<double, 2> center_offset{0, 0}; // Hugin d / width, e / height.

  bool operator==(const StitchInputLens& other) const {
    return projection == other.projection && optimization == other.optimization && distortion == other.distortion &&
        center_offset == other.center_offset;
  }
  bool operator!=(const StitchInputLens& other) const {
    return !(*this == other);
  }
};

absl::Status ValidateStitchInputLens(const StitchInputLens& lens);
absl::StatusOr<StitchInputLens> ReadStitchInputLens(const YAML::Node& node, const StitchInputLens& defaults = {});
YAML::Node WriteStitchInputLens(const StitchInputLens& lens);
std::string SerializeStitchInputLens(const StitchInputLens& lens);
absl::StatusOr<StitchInputLens> ParseStitchInputLens(const std::string& text);
int StitchInputLensProjection(const StitchInputLens& lens);

// Applies linked intrinsics to both original source images and, for explicit
// optimization modes, replaces the optimizer variables. Camera poses still fit
// the matches; fixed optics never disable alignment.
absl::StatusOr<std::string> ApplyStitchInputLens(const std::string& pto, const StitchInputLens& lens);

} // namespace hm::stitching
