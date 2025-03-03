#pragma once

#include "hstream/libs/common/Surface.h"

#include "absl/status/status.h"

#include <string>

namespace hm {
namespace stitching {

absl::Status configure_stitching(
    const std::string& game_id,
    surface::Surface left_surface,
    surface::Surface right_surface);

}
} // namespace hm
