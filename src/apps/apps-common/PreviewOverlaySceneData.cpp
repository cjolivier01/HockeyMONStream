#include "PreviewOverlayScene.h"

#include <cmath>
#include <limits>

namespace hm::gpu_preview {

std::optional<std::uint32_t> ExactPreviewDimension(float dimension) {
  const double exact_dimension = static_cast<double>(dimension);
  if (!std::isfinite(dimension) || dimension <= 0.0F ||
      exact_dimension > static_cast<double>(std::numeric_limits<std::uint32_t>::max()) ||
      std::floor(exact_dimension) != exact_dimension) {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(dimension);
}

} // namespace hm::gpu_preview
