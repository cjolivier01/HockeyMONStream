#pragma once

#include "hstream/src/libs/common/Surface.h"

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

#include <string>

namespace hm {
namespace stitching {

struct Synchronization {
  // Actual frame number
  double video1_frame_offset{0};
  // Actual frame number
  double video2_frame_offset{0};
};

absl::StatusOr<Synchronization> calculate_stitching_synchronization(
    const std::string& video1,
    const std::string& video2);

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir);

bool can_configure_stitching(const YAML::Node& config);

absl::Status create_field_mask(const std::string& game_dir, surface::Surface surface);

absl::Status configure_orientation(const std::string& game_dir);

absl::Status configure_stitching(
    const std::string& game_dir,
    surface::Surface left_surface,
    surface::Surface right_surface);

} // namespace stitching
} // namespace hm
