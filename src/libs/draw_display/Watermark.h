#pragma once

#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

#include <algorithm>
#include <string_view>

namespace hm::draw_display {

// HockeyMON's legacy mark occupies a fixed 319x111 image, with a right margin
// of one tenth of its width and no bottom margin. Keep those output-pixel
// dimensions while using the shared GPU glyph atlas instead of a frame copy.
inline bool AppendWatermark(
    analytics::CommandList* commands,
    float width,
    float height,
    float coordinate_per_output_pixel = 1.0F) {
  if (!commands || width <= 0 || height <= 0 || coordinate_per_output_pixel <= 0)
    return false;
  constexpr float kWidth = 319.0F;
  constexpr float kHeight = 111.0F;
  constexpr float kRightMargin = kWidth / 10.0F;
  const float scale = std::min({coordinate_per_output_pixel, width / (kWidth + kRightMargin), height / kHeight});
  const float left = width - (kWidth + kRightMargin) * scale;
  const float top = height - kHeight * scale;
  constexpr analytics::Color kRed{1.0F, 0.0F, 0.0F, 0.4F};
  constexpr float kGlyphHeight = 48.0F;
  auto line = [&](float y, std::string_view value) {
    const float line_width = value.size() * kGlyphHeight * (2.0F / 3.0F);
    return commands->AddText(
        left + (kWidth - line_width) * 0.5F * scale,
        top + y * scale,
        kGlyphHeight * scale,
        value,
        kRed,
        analytics::TextWeight::kBold);
  };
  const bool first = line(8.0F, "SportsAI");
  return line(55.0F, "HockeyMON") && first;
}

} // namespace hm::draw_display
