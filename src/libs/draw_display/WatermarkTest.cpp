#include "hstream/src/libs/draw_display/Watermark.h"
#include "hstream/src/libs/draw_display/AnalyticsOverlayAtlas.h"

#include <algorithm>
#include <iostream>
#include <vector>

int main() {
  using hm::draw_display::analytics::CommandList;
  CommandList commands;
  if (!hm::draw_display::AppendWatermark(&commands, 1920, 1080) || commands.empty()) {
    std::cerr << "Watermark emitted no glyphs\n";
    return 1;
  }
  if (commands.size() != 17 || commands.data()[0].glyph != 96 + 'S' - 32 || commands.data()[8].glyph != 96 + 'H' - 32) {
    std::cerr << "Watermark stroke weight changed\n";
    return 1;
  }
  using namespace hm::draw_display::analytics::detail;
  std::vector<uint8_t> atlas(kAtlasBytes);
  if (!BuildAtlas(atlas.data(), {})) {
    std::cerr << "Watermark font atlas could not be built\n";
    return 1;
  }
  unsigned regular_pixels = 0, bold_pixels = 0;
  const int glyph = 'S' - kFirstGlyph;
  for (int y = 0; y < kGlyphHeight; ++y)
    for (int x = 0; x < kGlyphWidth; ++x) {
      const int regular_y = (glyph / kAtlasColumns) * kGlyphHeight + y;
      const int regular_x = (glyph % kAtlasColumns) * kGlyphWidth + x;
      regular_pixels += atlas[regular_y * kAtlasWidth + regular_x] > 0;
      bold_pixels += atlas[(regular_y + 6 * kGlyphHeight) * kAtlasWidth + regular_x] > 0;
    }
  if (bold_pixels <= regular_pixels) {
    std::cerr << "Watermark glyph was not thickened\n";
    return 1;
  }
  float left = 1920, top = 1080, right = 0, bottom = 0;
  for (size_t index = 0; index < commands.size(); ++index) {
    const auto& command = commands.data()[index];
    left = std::min(left, command.x0);
    top = std::min(top, command.y0);
    right = std::max(right, command.x1);
    bottom = std::max(bottom, command.y1);
    if (command.color.red != 1.0F || command.color.green != 0.0F || command.color.alpha < 0.29F) {
      std::cerr << "Watermark color changed\n";
      return 1;
    }
  }
  if (left < 1920 - 319 - 32 || right > 1920 - 31 || top < 1080 - 111 || bottom > 1080) {
    std::cerr << "Watermark escaped its fixed bottom-right rectangle\n";
    return 1;
  }
  const size_t watermark_count = commands.size();
  if (!commands.AddFill(1600, 1000, 1700, 1050, {0, 1, 0, 1}) || !commands.MovePrefixToEnd(watermark_count) ||
      commands.data()[0].kind != Kind::kFill || commands.data()[1].glyph != 96 + 'S' - 32 ||
      commands.MovePrefixToEnd(commands.size() + 1)) {
    std::cerr << "Watermark did not retain draw priority after optional graphics\n";
    return 1;
  }
  commands.Clear();
  if (!hm::draw_display::AppendWatermark(&commands, 1600, 900, 2.0F)) {
    std::cerr << "Scaled preview watermark was rejected\n";
    return 1;
  }
  for (size_t index = 0; index < commands.size(); ++index) {
    const auto& command = commands.data()[index];
    if (command.x0 < 0 || command.y0 < 0 || command.x1 > 1600 || command.y1 > 900) {
      std::cerr << "Scaled preview watermark escaped the frame\n";
      return 1;
    }
  }
  return 0;
}
