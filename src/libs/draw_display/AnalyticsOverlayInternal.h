#pragma once
#include "hstream/src/libs/draw_display/AnalyticsOverlay.h"

namespace hm::draw_display::analytics::detail {
inline constexpr int kFirstGlyph = 32;
inline constexpr int kGlyphCount = 95;
inline constexpr int kGlyphWidth = 32;
inline constexpr int kGlyphHeight = 48;
inline constexpr int kAtlasColumns = 16;
inline constexpr int kAtlasWidth = kGlyphWidth * kAtlasColumns;
inline constexpr int kAtlasHeight = kGlyphHeight * 6;
inline constexpr size_t kAtlasBytes = kAtlasWidth * kAtlasHeight;
static_assert(kAtlasBytes <= kMaximumAtlasBytes);
static_assert(sizeof(Command) == 48);
static_assert(sizeof(Tile) == 16);
cudaError_t Raster(
    const ImageView& image,
    const Command* commands,
    const Tile* tiles,
    const uint32_t* references,
    uint32_t tile_count,
    const uint8_t* atlas,
    cudaStream_t stream);
} // namespace hm::draw_display::analytics::detail
