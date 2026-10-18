#pragma once

#include <string_view>

namespace hm::draw_display {
struct RinkMiniMapOptions {
  enum class Size { kIcon, kCompact, kStandard, kLarge };
  enum class Position { kUpperRight, kUpperLeft, kLowerLeft };
  Size size{Size::kStandard};
  Position position{Position::kUpperRight};
};

inline RinkMiniMapOptions RinkMiniMapPreferences(std::string_view size, std::string_view position) {
  return {
      size == "icon"          ? RinkMiniMapOptions::Size::kIcon
          : size == "compact" ? RinkMiniMapOptions::Size::kCompact
          : size == "large"   ? RinkMiniMapOptions::Size::kLarge
                              : RinkMiniMapOptions::Size::kStandard,
      position == "upper-left"       ? RinkMiniMapOptions::Position::kUpperLeft
          : position == "lower-left" ? RinkMiniMapOptions::Position::kLowerLeft
                                     : RinkMiniMapOptions::Position::kUpperRight};
}
} // namespace hm::draw_display
