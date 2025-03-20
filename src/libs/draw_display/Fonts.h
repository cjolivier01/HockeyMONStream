#pragma once

#include <cuda_runtime.h>
#include <memory>
#include <string>
#include <utility>
#include "absl/status/statusor.h"

namespace hm {
namespace draw_display {

/**
 * @brief Interface for a font used for rendering text onto a CUDA surface.
 *
 * This interface provides methods for loading a font and drawing text onto
 * an image surface. The drawing method returns the final drawn text dimensions.
 */
struct Font {
  virtual ~Font() = default;

  /**
   * @brief Loads the font into memory and prepares it for drawing.
   *
   * Must be called before any draw operations.
   *
   * @return absl::Status indicating success or failure.
   */
  virtual absl::Status load() = 0;

  /**
   * @brief Draws the given text string onto a CUDA surface.
   *
   * The text is drawn starting at (dest_x, dest_y) on the provided surface.
   * The method processes the text character-by-character. Newlines and spaces
   * are handled in a simple manner (without kerning or advanced layout).
   *
   * @param text The text string to draw.
   * @param surface Pointer to the image surface (CUDA memory) to draw on.
   * @param imgWidth Width of the image.
   * @param imgHeight Height of the image.
   * @param dest_x X-coordinate (in pixels) where text drawing begins.
   * @param dest_y Y-coordinate (in pixels) where text drawing begins.
   * @param textColor Color (uchar4) for the rendered text.
   * @return absl::StatusOr a pair (final_x, final_y) representing the end position
   *         after drawing the text.
   */
  virtual absl::StatusOr<std::pair<int, int>> draw(
      const std::string& text,
      void* surface,
      int imgWidth,
      int imgHeight,
      int dest_x,
      int dest_y,
      const uchar4& textColor) = 0;
};

/**
 * @brief Interface for a font cache.
 *
 * The FontCache manages creation and reuse of Font objects based on font file
 * and pixel height.
 */
struct FontCache {
  virtual ~FontCache() = default;

  /**
   * @brief Returns a cached Font instance or creates a new one if needed.
   *
   * If the font with the specified file path and pixel height is already in the cache,
   * the cached instance is returned. Otherwise, a new Font is created and cached.
   * If lazy_load is true, the font is not immediately loaded.
   *
   * @param font_path Path to the TrueType font file.
   * @param pixel_height Desired pixel height for the font.
   * @param lazy_load Whether to defer actual font loading.
   * @return absl::StatusOr containing a shared pointer to a Font instance.
   */
  virtual absl::StatusOr<std::shared_ptr<Font>> get_or_create_font(
      const std::string& font_path,
      int pixel_height,
      bool lazy_load = true) = 0;
};

/**
 * @brief Creates a new FontCache instance.
 *
 * @return A unique_ptr to a FontCache.
 */
std::shared_ptr<FontCache> create_font_cache();

} // namespace draw_display
} // namespace hm
