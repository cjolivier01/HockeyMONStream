#include "hstream/src/libs/detection/TileInferenceConfig.h"
#include <iostream>
#include <stdexcept>

int main() {
  try {
    auto enabled = hm::detection::ReadTileInferenceSettings(YAML::Load("{enable: 1, overlap: 0.3, merge-iou: 0.6}"));
    if (!enabled.ok() || !*enabled || (**enabled).overlap != .3 || (**enabled).merge_iou != .6f)
      throw std::runtime_error("Explicit tile settings lost");
    auto disabled = hm::detection::ReadTileInferenceSettings(YAML::Load("{enable: 0, overlap: 0.2, merge-iou: 0.5}"));
    if (!disabled.ok() || *disabled)
      throw std::runtime_error("Disabled tiling enabled");
    for (int tiles : {2, 4}) {
      auto saved = YAML::Load("{enable: 1, overlap: 0.3, merge-iou: 0.6}");
      saved["tiles"] = tiles;
      const auto restored = hm::detection::ReadTileInferenceSettings(saved);
      if (!restored.ok() || !*restored || (**restored).overlap != .3 || (**restored).merge_iou != .6f)
        throw std::runtime_error("Retired quadrant selector broke the saved two-half preference");
      const auto plan = hm::detection::MakeDetectionTiles(1000, 800, (**restored).overlap);
      if (!plan.ok() || plan->size() != 2 || (*plan)[0].height != 800 || (*plan)[1].top != 0)
        throw std::runtime_error("Retired quadrant selector restored four-part geometry");
    }
    for (const char* invalid :
         {"{}",
          "{enable: 1}",
          "{enable: 2, overlap: 0.2, merge-iou: 0.5}",
          "{enable: 1, overlap: 0.2, merge-iou: 0.5, tiles: 3}",
          "{enable: 1, overlap: 1, merge-iou: 0.5}",
          "{enable: 1, overlap: 0.2, merge-iou: .nan}",
          "{enable: 1, overlap: 0.2, merge-iou: 0.5, typo: 1}"})
      if (hm::detection::ReadTileInferenceSettings(YAML::Load(invalid)).ok())
        throw std::runtime_error("Invalid tile settings accepted");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
