#pragma once

#include <cstdint>

namespace hm::detection {

struct DetectionTile {
  int left, top, width, height;
};

struct Letterbox {
  int width, height;
  int resized_width, resized_height;
  int left, top;
  float scale;
};

struct Proposal {
  float left, top, right, bottom, confidence;
  int class_id;
  uint32_t source_index;
};

} // namespace hm::detection
