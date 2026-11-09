#include "hstream/src/libs/detection/TileInferenceConfig.h"
#include <iostream>
#include <stdexcept>

int main() {
  try {
    auto enabled =
        hm::detection::ReadTileInferenceSettings(YAML::Load("{enable: 1, tiles: 2, overlap: 0.3, merge-iou: 0.6}"));
    if (!enabled.ok() || !*enabled || (**enabled).overlap != .3 || (**enabled).merge_iou != .6f)
      throw std::runtime_error("Explicit tile settings lost");
    auto disabled =
        hm::detection::ReadTileInferenceSettings(YAML::Load("{enable: 0, tiles: 2, overlap: 0.2, merge-iou: 0.5}"));
    if (!disabled.ok() || *disabled)
      throw std::runtime_error("Disabled tiling enabled");
    auto quadrants =
        hm::detection::ReadTileInferenceSettings(YAML::Load("{enable: 1, tiles: 4, overlap: 0.2, merge-iou: 0.5}"));
    if (!quadrants.ok() || !*quadrants || (**quadrants).tiles != 4)
      throw std::runtime_error("Quadrant setting lost");
    for (const char* invalid :
         {"{}",
          "{enable: 1}",
          "{enable: 1, overlap: 0.2, merge-iou: 0.5}",
          "{enable: 1, tiles: 3, overlap: 0.2, merge-iou: 0.5}",
          "{enable: 0, tiles: 0, overlap: 0.2, merge-iou: 0.5}",
          "{enable: 1, tiles: 2.5, overlap: 0.2, merge-iou: 0.5}",
          "{enable: 2, tiles: 2, overlap: 0.2, merge-iou: 0.5}",
          "{enable: 1, tiles: 2, overlap: 1, merge-iou: 0.5}",
          "{enable: 1, tiles: 2, overlap: 0.2, merge-iou: .nan}",
          "{enable: 1, tiles: 2, overlap: 0.2, merge-iou: 0.5, typo: 1}"})
      if (hm::detection::ReadTileInferenceSettings(YAML::Load(invalid)).ok())
        throw std::runtime_error("Invalid tile settings accepted");
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
