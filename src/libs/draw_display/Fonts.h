#pragma once

#include <cuda_runtime.h>

#include <memory>

#include "absl/status/statusor.h"

namespace hm {
namespace draw_display {

struct Font {
  virtual ~Font() = default;
  virtual absl::Status load() = 0;
  virtual absl::StatusOr<std::pair<int, int>> draw(
      const std::string& text,
      void* surface,
      int imgWidth,
      int imgHeight,
      int dest_x,
      int dest_y,
      const uchar4& textColor) = 0;
};

struct FontCache {
  virtual ~FontCache() = default;
  virtual absl::StatusOr<std::shared_ptr<Font>> get_or_create_font(
      const std::string& font_path,
      int pixel_height,
      bool lazy_load = true) = 0;
};

std::unique_ptr<FontCache> create_font_cache();

} // namespace draw_display
} // namespace hm
