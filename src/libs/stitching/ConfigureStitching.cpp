#include "hstream/libs/stitching/ConfigureStitching.h"

namespace hm {
namespace stitching {

namespace {
//
}

absl::StatusOr<bool> is_stitching_configured(const std::string& game_dir) {
  return false;
}

bool can_configure_stitching(const YAML::Node& config) {
  return true;
}

absl::Status configure_stitching(
    const std::string& game_id,
    surface::Surface left_surface,
    surface::Surface right_surface) {
  return absl::OkStatus();
}

} // namespace stitching
} // namespace hm
