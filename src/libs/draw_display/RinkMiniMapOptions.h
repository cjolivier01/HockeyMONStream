#pragma once

#include <optional>
#include <string_view>

namespace hm::draw_display {
struct RinkMiniMapOptions {
  enum class Size { kIcon, kCompact, kStandard, kLarge };
  enum class Position { kUpperRight, kUpperLeft, kLowerLeft };
  Size size{Size::kStandard};
  Position position{Position::kUpperRight};
  // Which sweater the home side wore, when the game said. HOME and AWAY are
  // names for the two groups, not shades: with home in light sweaters the
  // home side must still be drawn light.
  std::optional<bool> home_is_dark;
};

inline RinkMiniMapOptions RinkMiniMapPreferences(
    std::string_view size,
    std::string_view position,
    std::optional<bool> home_is_dark = std::nullopt) {
  return {
      size == "icon"          ? RinkMiniMapOptions::Size::kIcon
          : size == "compact" ? RinkMiniMapOptions::Size::kCompact
          : size == "large"   ? RinkMiniMapOptions::Size::kLarge
                              : RinkMiniMapOptions::Size::kStandard,
      position == "upper-left"       ? RinkMiniMapOptions::Position::kUpperLeft
          : position == "lower-left" ? RinkMiniMapOptions::Position::kLowerLeft
                                     : RinkMiniMapOptions::Position::kUpperRight,
      home_is_dark};
}
} // namespace hm::draw_display
