#pragma once

#include "absl/status/status.h"

#include <string>

namespace hm {
namespace stitching {
absl::Status configure_stitching(const std::string& game_id);
}
} // namespace hm
\
