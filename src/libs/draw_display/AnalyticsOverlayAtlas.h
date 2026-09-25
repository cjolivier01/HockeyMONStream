#pragma once
#include "hstream/src/libs/draw_display/AnalyticsOverlayInternal.h"

#include <string>

namespace hm::draw_display::analytics::detail {
// Shared CPU atlas builder for CUDA and GL. Caller supplies kAtlasBytes storage.
// Reads one trusted installed monospace font; no GPU calls or persistent state.
bool BuildAtlas(uint8_t* atlas, const std::string& configured_path);
} // namespace hm::draw_display::analytics::detail
