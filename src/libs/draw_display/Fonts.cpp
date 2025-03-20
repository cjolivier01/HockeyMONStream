#include "hstream/src/libs/draw_display/Fonts.h"
#include <cuda_runtime.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <unordered_map>
#include <vector>
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/draw_display/cudaDrawText.h" // Provides drawGlyph(...)

// stb_truetype: include the header and define its implementation in this translation unit.
#define STB_TRUETYPE_IMPLEMENTATION
#include "hstream/src/libs/draw_display/stb_truetype.h"

#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

namespace fs = std::filesystem;

namespace hm {
namespace draw_display {

namespace {

/**
 * @brief Trims whitespace from both ends of a string.
 *
 * @param s Input string.
 * @return A trimmed version of the input string.
 */
static inline std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\r\n");
  auto end = s.find_last_not_of(" \t\r\n");
  return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
}

/**
 * @brief Returns a map of font family names to font file paths by running fc-list.
 *
 * This function calls fc-list with options to display the font file and family.
 * It then parses each line of the output (expected format: "path: family")
 * and returns a std::map where the key is the font family name and the value is
 * the file path to the font.
 *
 * @return std::map<std::string, std::string> mapping font family name to file path.
 */
std::map<std::string, std::string> getFontMap() {
  std::map<std::string, std::string> fontMap;

  // fc-list with "file" and "family" fields (colon-separated)
  // Example output line:
  //   /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf: DejaVu Sans
  const char* command = "fc-list : file family";
  FILE* pipe = popen(command, "r");
  if (!pipe) {
    std::cerr << "Error: Could not run fc-list command." << std::endl;
    return fontMap;
  }

  char buffer[2048];
  while (fgets(buffer, sizeof(buffer), pipe)) {
    std::string line(buffer);
    // Remove any trailing newline.
    line = trim(line);
    if (line.empty()) {
      continue;
    }

    // Expect line format: "<file_path>: <family>"
    size_t colonPos = line.find(':');
    if (colonPos == std::string::npos) {
      continue;
    }

    std::string filePath = trim(line.substr(0, colonPos));
    std::string family = trim(line.substr(colonPos + 1));
    if (!filePath.empty() && !family.empty()) {
      // Insert into map.
      fontMap[family] = filePath;
    }
  }

  pclose(pipe);
  return fontMap;
}

/**
 * @brief A simple RAII wrapper for a CUDA-allocated memory buffer.
 */
struct CudaBuffer {
  CudaBuffer() = default;
  CudaBuffer(void* d, size_t s) : data(d), size(s) {}
  // Do not allow move semantics here.
  CudaBuffer(CudaBuffer&& other) = delete;
  ~CudaBuffer() {
    if (data) {
      cudaFree(data);
    }
  }
  void assign(void* d, size_t s) {
    data = d;
    size = s;
  }
  void* data{nullptr};
  size_t size{0};
};

/**
 * @brief Structure representing the dimensions of a glyph.
 */
struct FontSize {
  int width{0};
  int height{0};
};

/**
 * @brief Encapsulates a single glyph for a font.
 *
 * Loads the glyph bitmap for a given Unicode codepoint using stb_truetype,
 * uploads the bitmap to device memory, and provides a draw() method to overlay
 * the glyph onto an image surface.
 */
struct FontGlyph {
  /**
   * @brief Constructs a FontGlyph for the given codepoint.
   *
   * @param codepoint Unicode codepoint for the glyph.
   */
  FontGlyph(int codepoint) : codepoint_(codepoint) {}

  /**
   * @brief Loads the glyph bitmap using the provided font info and scale.
   *
   * The bitmap is generated using stb_truetype and then copied to device memory.
   *
   * @param font Pointer to an initialized stbtt_fontinfo.
   * @param scale Scale factor computed for the desired pixel height.
   * @return absl::Status indicating success or failure.
   */
  absl::Status load(stbtt_fontinfo* font, float scale) {
    // Generate a grayscale bitmap for the glyph.
    glyphBitmap_ =
        stbtt_GetCodepointBitmap(font, 0, scale, codepoint_, &glyphWidth_, &glyphHeight_, &xoffset_, &yoffset_);
    if (!glyphBitmap_) {
      return absl::InternalError(
          TO_STRING("Error: Could not generate bitmap for character '" << char(codepoint_) << "'."));
    }
    // Allocate device memory for the glyph bitmap.
    unsigned char* d_glyph;
    glyphSize_ = glyphWidth_ * glyphHeight_ * sizeof(unsigned char);
    cudaError_t err = cudaMalloc(&d_glyph, glyphSize_);
    if (err != cudaSuccess) {
      return absl::InternalError(TO_STRING("CUDA Error (cudaMalloc for glyph): " << cudaGetErrorString(err)));
    }
    cuda_buffer_.assign(d_glyph, glyphSize_);
    // Copy the host bitmap to the device.
    cudaMemcpy(d_glyph, glyphBitmap_, glyphSize_, cudaMemcpyHostToDevice);
    return absl::OkStatus();
  }

  /**
   * @brief Draws the glyph onto the provided image surface.
   *
   * Uses the drawGlyph() function (which launches a CUDA kernel) to overlay the glyph.
   *
   * @param surface Pointer to the image surface (CUDA memory).
   * @param imgWidth Width of the image.
   * @param imgHeight Height of the image.
   * @param dest_x X-coordinate on the image where the glyph should be drawn.
   * @param dest_y Y-coordinate on the image where the glyph should be drawn.
   * @param textColor Color to draw the glyph (as an uchar4).
   * @return cudaError_t from the underlying kernel launch.
   */
  cudaError_t draw(void* surface, int imgWidth, int imgHeight, int dest_x, int dest_y, uchar4 textColor) {
    if (!cuda_buffer_.data) {
      // Should never happen if load() succeeded.
      assert(false);
      return cudaErrorUnknown;
    }
    // Use a threshold of 128 to decide which pixels to draw.
    return drawGlyph(
        reinterpret_cast<uchar4*>(surface),
        imgWidth,
        imgHeight,
        reinterpret_cast<const uint8_t*>(cuda_buffer_.data),
        glyphWidth_,
        glyphHeight_,
        dest_x,
        dest_y,
        textColor,
        /*threshold=*/128,
        /*stream=*/nullptr);
  }

  /**
   * @brief Returns the dimensions of the glyph.
   *
   * @return A FontSize structure with width and height.
   */
  FontSize size() const {
    return FontSize{
        .width = glyphWidth_,
        .height = glyphHeight_,
    };
  }

  ~FontGlyph() {
    // Free the host-side bitmap.
    if (glyphBitmap_) {
      stbtt_FreeBitmap(glyphBitmap_, nullptr);
    }
  }

 private:
  int codepoint_;
  int glyphWidth_{0}, glyphHeight_{0}, xoffset_{0}, yoffset_{0};
  uint8_t* glyphBitmap_{nullptr};
  CudaBuffer cuda_buffer_;
  size_t glyphSize_{0};
};

/**
 * @brief Private implementation of the Font interface.
 *
 * Loads a TrueType font file, initializes stb_truetype, and manages caching of
 * individual glyphs.
 */
class FontImpl : public Font {
 public:
  /**
   * @brief Constructs a FontImpl with the specified font file and pixel height.
   *
   * @param font_path Path to the TrueType font file.
   * @param pixel_height Desired pixel height.
   */
  FontImpl(const std::string& font_path, int pixel_height) : font_path_(font_path), pixel_height_(pixel_height) {
    if (!fs::exists(font_path)) {
      std::cerr << "Warning: Could not find font file: " << font_path << std::endl;
    }
  }

  /**
   * @brief Loads the font from file and initializes stb_truetype.
   *
   * @return absl::Status indicating success or failure.
   */
  absl::Status load() override {
    //--------------------------------------------------------------------------
    // 1. Load the TrueType font file.
    //--------------------------------------------------------------------------
    std::ifstream font_file(font_path_, std::ios::binary);
    if (!font_file) {
      return absl::NotFoundError(TO_STRING("Error: Could not open font file: " << font_path_));
    }
    std::vector<unsigned char> font_buffer(
        (std::istreambuf_iterator<char>(font_file)), std::istreambuf_iterator<char>());
    font_file.close();

    //--------------------------------------------------------------------------
    // 2. Initialize stb_truetype.
    //--------------------------------------------------------------------------
    if (!stbtt_InitFont(&font_, font_buffer.data(), 0)) {
      return absl::InternalError("Error: Could not initialize font.");
    }
    // Compute the scale factor for the desired pixel height.
    scale_ = stbtt_ScaleForPixelHeight(&font_, pixel_height_);
    return absl::OkStatus();
  }

  /**
   * @brief Draws a single character onto the image surface.
   *
   * This method renders one character (specified by its integer codepoint),
   * draws it at the given destination coordinates, and returns the glyph size.
   *
   * @param character Unicode codepoint of the character.
   * @param surface Pointer to the image surface (CUDA memory).
   * @param imgWidth Width of the image.
   * @param imgHeight Height of the image.
   * @param dest_x X-coordinate for drawing the glyph.
   * @param dest_y Y-coordinate for drawing the glyph.
   * @param textColor Color of the text (uchar4).
   * @return absl::StatusOr containing a FontSize (width, height) of the drawn glyph.
   */
  absl::StatusOr<FontSize> draw(
      int character,
      void* surface,
      int imgWidth,
      int imgHeight,
      int dest_x,
      int dest_y,
      const uchar4& textColor) {
    // If a previous error occurred, return that status.
    if (!status_.ok()) {
      return status_;
    }
    // Ensure the font is loaded (this call is thread-safe).
    if (!loaded_) {
      absl::MutexLock lk(&mu_);
      if (!loaded_) {
        status_.Update(load());
        if (status_.ok()) {
          loaded_ = true;
        }
      }
    }
    FontGlyph* g;
    HM_ASSIGN_OR_RETURN(g, get_or_crerate_glyph(character));
    cudaError_t cerr = g->draw(surface, imgWidth, imgHeight, dest_x, dest_y, textColor);
    if (cerr != cudaError_t::cudaSuccess) {
      return to_status(cerr);
    }
    return g->size();
  }

  /**
   * @brief Draws the given text string onto the image surface.
   *
   * Processes the text character-by-character. Newlines reset the x position
   * and increment the y position by the maximum character height in that line.
   * Spaces use the width of the space character.
   *
   * @param text The text string to render.
   * @param surface Pointer to the image surface.
   * @param imgWidth Width of the image.
   * @param imgHeight Height of the image.
   * @param dest_x Starting x-coordinate.
   * @param dest_y Starting y-coordinate.
   * @param textColor Color for the text.
   * @return absl::StatusOr containing a pair (final_x, final_y) for the end position.
   */
  absl::StatusOr<std::pair<int, int>> draw(
      const std::string& text,
      void* surface,
      int imgWidth,
      int imgHeight,
      int dest_x,
      int dest_y,
      const uchar4& textColor) override {
    int pos_x = dest_x;
    int pos_y = dest_y;
    int max_height = 0;
    int space_width = 0;
    FontSize last_size{.width = 0, .height = 0};
    // Process each character in the text.
    for (const auto& c : text) {
      if (c == '\n') {
        // Newline: reset x and move y by the current maximum height.
        pos_x = dest_x;
        pos_y += max_height;
        max_height = 0;
      } else if (c == ' ') {
        // Space: if known width, advance; otherwise draw to measure.
        if (space_width) {
          pos_x += space_width;
        } else {
          HM_ASSIGN_OR_RETURN(last_size, draw(c, surface, imgWidth, imgHeight, pos_x, pos_y, textColor));
          space_width = last_size.width;
          pos_x += last_size.width;
        }
      } else if (c == '\r') {
        // Carriage return: reset x position.
        pos_x = dest_x;
      } else {
        // Draw the character and advance x by the glyph width.
        HM_ASSIGN_OR_RETURN(last_size, draw(c, surface, imgWidth, imgHeight, pos_x, pos_y, textColor));
        pos_x += last_size.width;
      }
      // Update max height for the current line.
      max_height = std::max(max_height, last_size.height);
    }
    return std::make_pair(pos_x, pos_y);
  }

 protected:
  /**
   * @brief Retrieves a cached glyph or creates it if not present.
   *
   * This method is thread-safe.
   *
   * @param codepoint Unicode codepoint for the desired glyph.
   * @return absl::StatusOr containing a pointer to a FontGlyph.
   */
  absl::StatusOr<FontGlyph*> get_or_crerate_glyph(int codepoint) {
    absl::MutexLock lk(&mu_);
    auto found = glyphs_.find(codepoint);
    if (found != glyphs_.end()) {
      return found->second.get();
    }
    // Create a new glyph.
    auto glyph = std::make_unique<FontGlyph>(codepoint);
    HM_RETURN_IF_ERROR(glyph->load(&font_, scale_));
    auto iter = glyphs_.emplace(codepoint, std::move(glyph)).first;
    return iter->second.get();
  }

 private:
  std::string font_path_;
  int pixel_height_;
  float scale_{0.0};
  stbtt_fontinfo font_;
  absl::Mutex mu_;
  bool loaded_ ABSL_GUARDED_BY(mu_){false};
  // Map from Unicode codepoint to its cached glyph.
  std::unordered_map<int, std::unique_ptr<FontGlyph>> glyphs_ ABSL_GUARDED_BY(mu_);
  absl::Status status_;
};

/**
 * @brief Private implementation of the FontCache interface.
 *
 * Caches FontImpl objects based on font file path and pixel height.
 */
class FontCacheImpl : public FontCache {
 public:
  FontCacheImpl() {
    std::map<std::string, std::string> font_name_to_file_ = getFontMap();
  }

  /**
   * @brief Retrieves a cached Font instance or creates a new one.
   *
   * @param font_path Path to the font file.
   * @param pixel_height Desired pixel height.
   * @param lazy_load If true, the font is not immediately loaded.
   * @return absl::StatusOr containing a shared pointer to a Font instance.
   */
  absl::StatusOr<std::shared_ptr<Font>> get_or_create_font(
      const std::string& font_name_or_path,
      int pixel_height,
      bool lazy_load) override {
    const std::string* font_file_path = &font_name_or_path;
    auto found_name = font_name_to_file_.find(*font_file_path);
    if (found_name != font_name_to_file_.end()) {
      font_file_path = &found_name->second;
    }

    absl::MutexLock lk(&mu_);
    std::pair<int, std::string> key = std::make_pair(pixel_height, *font_file_path);
    auto found = font_cache_.find(key);
    if (found != font_cache_.end()) {
      return found->second;
    }
    // Create a new FontImpl and cache it.
    found = font_cache_.emplace(std::move(key), std::make_shared<FontImpl>(*font_file_path, pixel_height)).first;
    // If not lazy loading, load the font immediately.
    if (!lazy_load) {
      HM_RETURN_IF_ERROR(found->second->load());
    }
    return found->second;
  }

 private:
  std::map<std::string, std::string> font_name_to_file_;
  absl::Mutex mu_;
  // Map from (pixel_height, font_path) to FontImpl.
  std::map<std::pair<int, std::string>, std::shared_ptr<FontImpl>> font_cache_ ABSL_GUARDED_BY(mu_);
};

absl::Mutex weak_font_cache_ptr_mu;
} // end anonymous namespace

/**
 * @brief Factory function to create a FontCache.
 *
 * @return std::unique_ptr to a FontCache instance.
 */
std::shared_ptr<FontCache> get_or_create_font_cache(bool create_if_needed) {
  absl::MutexLock lk(&weak_font_cache_ptr_mu);
  static std::weak_ptr<FontCache> weak_font_cache_ptr;
  auto sp = weak_font_cache_ptr.lock();
  if (sp) {
    return sp;
  } else if (!create_if_needed) {
    return nullptr;
  }
  sp = std::make_shared<FontCacheImpl>();
  weak_font_cache_ptr = sp;
  return sp;
}

} // namespace draw_display
} // namespace hm
