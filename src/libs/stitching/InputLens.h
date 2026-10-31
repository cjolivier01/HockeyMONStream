#pragma once

#include <array>
#include <map>
#include <optional>
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

// A complete source-camera prescription. Zero FOV is an invalid sentinel;
// these values must come from resolved configuration or recording metadata.
struct StitchCameraOptics {
  double horizontal_fov{0};
  double vertical_fov{0};
  StitchInputLens input_lens;
  // Optional original recording dimensions survive frozen snapshots without
  // opening source media. They do not change the solved intrinsics.
  std::optional<std::array<int, 2>> source_size;
  // Absent controls inherit the shared preset for compatibility with existing
  // games. Newly selected cameras retain their own profile and Auto policies.
  std::string configuration;
  std::optional<bool> auto_fov;
  std::optional<bool> auto_lens;
  std::optional<bool> frozen;
  bool operator==(const StitchCameraOptics& other) const {
    return horizontal_fov == other.horizontal_fov && vertical_fov == other.vertical_fov &&
        input_lens == other.input_lens && source_size == other.source_size;
  }
  bool operator!=(const StitchCameraOptics& other) const {
    return !(*this == other);
  }
};

using StitchInputCameras = std::map<std::string, StitchCameraOptics>;
absl::Status ValidateStitchCameraOptics(const StitchCameraOptics& camera);
absl::StatusOr<StitchInputCameras> ReadStitchInputCameras(const YAML::Node& node);
YAML::Node WriteStitchInputCameras(const StitchInputCameras& cameras, bool include_policies = true);
std::string SerializeStitchInputCameras(const StitchInputCameras& cameras);
absl::StatusOr<StitchInputCameras> ParseStitchInputCameras(const std::string& text);

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

// Independent intrinsics are never linked between the source images. Every
// camera retains its own FOV, distortion, center and optimization policy.
absl::StatusOr<std::string> ApplyStitchInputCameras(
    const std::string& pto,
    const std::array<StitchCameraOptics, 2>& cameras);

} // namespace hm::stitching
