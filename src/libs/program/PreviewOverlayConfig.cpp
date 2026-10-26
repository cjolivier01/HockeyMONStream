#include "PreviewOverlayConfig.h"

namespace hm::pipeline_internal {
absl::StatusOr<PreviewOverlaySelection> ReadPreviewOverlaySelection(const YAML::Node& resolved) {
  try {
    const auto node = resolved["preview_overlays"];
    if (!node || !node.IsMap())
      return absl::InvalidArgumentError("Missing resolved preview_overlays configuration");
    PreviewOverlaySelection selection;
    const auto read = [&](const char* key) {
      if (!node[key] || !node[key].IsScalar())
        throw std::runtime_error(std::string("Missing preview_overlays.") + key);
      return node[key].as<bool>();
    };
    selection.players = read("players");
    selection.play = read("play");
    selection.rink = read("rink");
    return selection;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid preview overlay selection: ") + error.what());
  }
}
} // namespace hm::pipeline_internal
