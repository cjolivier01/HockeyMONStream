#pragma once

#include "deepstream/sources/includes/nvdsmeta.h"
#include "hstream/src/libs/common/Surface.h"

#include "absl/status/status.h"

namespace hm {
namespace draw_display {

absl::Status draw_display_meta(surface::Surface surface, const NvDsDisplayMeta* display_meta, cudaStream_t stream);

} // namespace draw_display
} // namespace hm
