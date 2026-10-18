#pragma once

#include <string_view>

namespace hm::draw_display {
struct RinkMiniMapOptions {
  enum class Size { kCompact, kStandard, kLarge };
  Size size{Size::kStandard};
  bool upper_left{false};
};

inline RinkMiniMapOptions RinkMiniMapPreferences(std::string_view size, std::string_view position) {
  return {
      size == "compact"     ? RinkMiniMapOptions::Size::kCompact
          : size == "large" ? RinkMiniMapOptions::Size::kLarge
                            : RinkMiniMapOptions::Size::kStandard,
      position == "upper-left"};
}
} // namespace hm::draw_display
