#pragma once

#include "hstream/libs/common/Surface.h"

#include "absl/status/statusor.h"

#include <string>

namespace hm {
namespace stitching {

absl::StatusOr<bool> is_stitching_configured(const std::string& game_id);

absl::Status configure_stitching(
    const std::string& game_id,
    surface::Surface left_surface,
    surface::Surface right_surface);

}
} // namespace hm
