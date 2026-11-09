#include "hstream/src/libs/detection/TileInferenceConfig.h"

namespace hm::detection {
absl::StatusOr<std::optional<TileInferenceSettings>> ReadTileInferenceSettings(const YAML::Node& node) {
  try {
    if (!node.IsMap())
      return absl::InvalidArgumentError("Missing tiled-inference configuration from baseline");
    for (const auto& entry : node) {
      const auto key = entry.first.as<std::string>();
      if (key != "enable" && key != "tiles" && key != "overlap" && key != "merge-iou")
        return absl::InvalidArgumentError("Unknown tiled-inference setting: " + key);
    }
    const int enabled = node["enable"].as<int>();
    if (enabled != 0 && enabled != 1)
      return absl::InvalidArgumentError("tiled-inference.enable must be 0 or 1");
    const TileInferenceSettings settings{
        node["overlap"].as<double>(), node["merge-iou"].as<float>(), node["tiles"].as<int>()};
    const auto status = ValidateTileInference(settings);
    if (!status.ok())
      return status;
    return enabled ? std::optional<TileInferenceSettings>(settings) : std::nullopt;
  } catch (const std::exception& e) {
    return absl::InvalidArgumentError(std::string("Invalid tiled-inference configuration: ") + e.what());
  }
}
} // namespace hm::detection
