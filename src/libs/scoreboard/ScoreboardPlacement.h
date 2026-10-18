#pragma once

#include <algorithm>
#include <optional>
#include <string_view>

namespace hm::scoreboard {
// Lowest to highest precedence within one configuration layer.
inline constexpr const char* kPositionPaths[] = {
    "rink.scoreboard.position",
    "pipeline.hmplaycropper.scoreboard_position",
    "pipeline.hmplaycropper.scoreboard-position",
    "pipeline.hmplaycropper.properties.scoreboard_position",
    "pipeline.hmplaycropper.properties.scoreboard-position",
    "pipeline.hmplaycropper.private-properties.scoreboard_position",
    "pipeline.hmplaycropper.private-properties.scoreboard-position"};

enum class Position { kUpperLeft, kUpperCenter, kUpperRight };

inline std::optional<Position> ParsePosition(std::string_view value) {
  if (value == "upper-left")
    return Position::kUpperLeft;
  if (value == "upper-center")
    return Position::kUpperCenter;
  if (value == "upper-right")
    return Position::kUpperRight;
  return std::nullopt;
}

inline int HorizontalOffset(Position position, int output_width, int scoreboard_width) {
  const int available = std::max(0, output_width - scoreboard_width);
  return position == Position::kUpperRight ? available : position == Position::kUpperCenter ? available / 2 : 0;
}
} // namespace hm::scoreboard
