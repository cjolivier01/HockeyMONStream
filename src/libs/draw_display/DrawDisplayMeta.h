#pragma once

#include "deepstream/sources/includes/nvdsmeta.h"

#include "absl/status/status.h"

namespace hm {
namespace draw_display {

absl::Status draw_display_meta(const NvDsDisplayMeta* display_meta);

} // namespace draw_display
} // namespace hm
