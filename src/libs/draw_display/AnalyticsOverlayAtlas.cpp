#include "hstream/src/libs/draw_display/AnalyticsOverlayAtlas.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <vector>

// Reuse the existing CPU rasterizer, with private symbols so Fonts.cpp remains
// independent. The legacy font renderer allocates/launches once per glyph.
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "hstream/src/libs/draw_display/stb_truetype.h"

namespace hm::draw_display::analytics::detail {
namespace {
constexpr size_t kMaximumFontBytes = 8 * 1024 * 1024;
}
bool BuildAtlas(uint8_t* atlas, const std::string& configured_path) {
  const char* candidates[] = {
      "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
      "/usr/share/fonts/truetype/liberation2/LiberationMono-Regular.ttf",
      "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf"};
  std::ifstream file;
  if (!configured_path.empty()) {
    file.open(configured_path, std::ios::binary | std::ios::ate);
  } else {
    for (const char* candidate : candidates) {
      file.open(candidate, std::ios::binary | std::ios::ate);
      if (file)
        break;
      file.clear();
    }
  }
  if (!file)
    return false;
  const auto length = file.tellg();
  if (length <= 0 || length > static_cast<std::streamoff>(kMaximumFontBytes))
    return false;
  std::vector<uint8_t> bytes(static_cast<size_t>(length));
  file.seekg(0);
  if (!file.read(reinterpret_cast<char*>(bytes.data()), bytes.size()))
    return false;
  stbtt_fontinfo font{};
  if (!stbtt_InitFont(&font, bytes.data(), 0))
    return false;
  // Leave a pixel for rounding above/below the advertised ascent/descent.
  const float scale = stbtt_ScaleForPixelHeight(&font, detail::kGlyphHeight - 2);
  int ascent, descent, gap;
  stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
  const int baseline = static_cast<int>(std::ceil(ascent * scale)) + 1;
  std::memset(atlas, 0, detail::kAtlasBytes);
  for (int index = 0; index < detail::kGlyphCount; ++index) {
    const int code = index + detail::kFirstGlyph;
    int x0, y0, x1, y1;
    stbtt_GetCodepointBitmapBox(&font, code, scale, scale, &x0, &y0, &x1, &y1);
    const int width = x1 - x0, height = y1 - y0;
    if (width == 0 || height == 0)
      continue;
    const int x = (detail::kGlyphWidth - width) / 2;
    const int y = baseline + y0;
    if (x < 0 || y < 0 || width > detail::kGlyphWidth || y + height > detail::kGlyphHeight)
      return false;
    const int cell_x = (index % detail::kAtlasColumns) * detail::kGlyphWidth;
    const int cell_y = (index / detail::kAtlasColumns) * detail::kGlyphHeight;
    stbtt_MakeCodepointBitmap(
        &font,
        atlas + (cell_y + y) * detail::kAtlasWidth + cell_x + x,
        width,
        height,
        detail::kAtlasWidth,
        scale,
        scale,
        code);
  }
  // Prepare a thicker variant once. Keeping it in the same atlas lets a bold
  // watermark use one glyph command per letter on every frame.
  for (int index = 0; index < detail::kGlyphCount; ++index) {
    const int cell_x = (index % detail::kAtlasColumns) * detail::kGlyphWidth;
    const int cell_y = (index / detail::kAtlasColumns) * detail::kGlyphHeight;
    for (int y = 0; y < detail::kGlyphHeight; ++y) {
      const uint8_t* regular = atlas + (cell_y + y) * detail::kAtlasWidth + cell_x;
      uint8_t* bold = atlas + (cell_y + y + detail::kGlyphHeight * 6) * detail::kAtlasWidth + cell_x;
      for (int x = 0; x < detail::kGlyphWidth; ++x)
        bold[x] = std::max({regular[x], regular[std::max(0, x - 1)], regular[std::max(0, x - 2)]});
    }
  }
  return true;
}
} // namespace hm::draw_display::analytics::detail
