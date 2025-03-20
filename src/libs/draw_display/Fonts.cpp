#include "hstream/src/libs/draw_display/Fonts.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/draw_display/cudaDrawText.h"

#include <cuda_runtime.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

// stb_truetype: include the header and define implementation in one file.
#define STB_TRUETYPE_IMPLEMENTATION
#include "hstream/src/libs/draw_display/stb_truetype.h"

#include "absl/status/statusor.h"
#include "absl/synchronization/mutex.h"

namespace fs = std::filesystem;

namespace hm {
namespace draw_display {

namespace {
struct CudaBuffer {
  CudaBuffer() = default;
  CudaBuffer(void* data, size_t size) : data(data), size(size) {}
  CudaBuffer(CudaBuffer&& other) = delete;
  ~CudaBuffer() {
    if (data) {
      cudaFree(data);
    }
  }
  void assign(void* d, size_t z) {
    data = d;
    size = z;
  }
  void* data{nullptr};
  size_t size{0};
};

struct FontSize {
  int width{0};
  int height{0};
};

struct FontGlyph {
  FontGlyph(int codepoint) : codepoint_(codepoint) {}

  absl::Status load(stbtt_fontinfo* font, float scale) {
    glyphBitmap_ =
        stbtt_GetCodepointBitmap(font, 0, scale, codepoint_, &glyphWidth_, &glyphHeight_, &xoffset_, &yoffset_);
    if (!glyphBitmap_) {
      return absl::InternalError(
          TO_STRING("Error: Could not generate bitmap for character '" << char(codepoint_) << "'."));
    }
    //--------------------------------------------------------------------------
    // 3. Copy the glyph bitmap to GPU memory.
    //--------------------------------------------------------------------------
    unsigned char* d_glyph;
    glyphSize_ = glyphWidth_ * glyphHeight_ * sizeof(unsigned char);
    cudaError_t err = cudaMalloc(&d_glyph, glyphSize_);
    if (err != cudaSuccess) {
      return absl::InternalError(TO_STRING("CUDA Error (cudaMalloc for glyph): " << cudaGetErrorString(err)));
    }
    cuda_buffer_.assign(d_glyph, glyphSize_);
    cudaMemcpy(d_glyph, glyphBitmap_, glyphSize_, cudaMemcpyHostToDevice);
    return absl::OkStatus();
  }

  cudaError_t draw(void* surface, int imgWidth, int imgHeight, int dest_x, int dest_y, uchar4 textColor) {
    if (!cuda_buffer_.data) {
      assert(false);
      return cudaError_t::cudaSuccess;
    }
    return drawGlyph(
        (uchar4*)surface,
        imgWidth,
        imgHeight,
        (const uint8_t*)cuda_buffer_.data,
        glyphWidth_,
        glyphHeight_,
        dest_x,
        dest_y,
        textColor,
        /*threshold=*/128,
        /*stream=*/nullptr);
  }

  FontSize size() const {
    return FontSize{
        .width = glyphWidth_,
        .height = glyphHeight_,
    };
  }

  ~FontGlyph() {
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

class FontImpl : public Font {
 public:
  FontImpl(const std::string& font_path, int pixel_height) : font_path_(font_path), pixel_height_(pixel_height) {
    if (!fs::exists(font_path)) {
      std::cerr << "Warning: Could not find font file: " << font_path << std::endl;
    }
  }

  absl::Status load() {
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
    // 2. Initialize stb_truetype and render a glyph.
    //--------------------------------------------------------------------------
    if (!stbtt_InitFont(&font_, font_buffer.data(), 0)) {
      return absl::InternalError("Error: Could not initialize font.");
    }

    // Set the desired pixel height (for example, 48 pixels tall).
    scale_ = stbtt_ScaleForPixelHeight(&font_, pixel_height_);
    return absl::OkStatus();
  }

  absl::StatusOr<FontSize> draw(
      int character,
      void* surface,
      int imgWidth,
      int imgHeight,
      int dest_x,
      int dest_y,
      const uchar4& textColor) {
    if (!status_.ok()) {
      return status_;
    }
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

  absl::StatusOr<std::pair<int, int>> draw(
      const std::string& text,
      void* surface,
      int imgWidth,
      int imgHeight,
      int dest_x,
      int dest_y,
      const uchar4& textColor) {
    int pos_x = dest_x;
    int pos_y = dest_y;
    int max_height = 0;
    int space_width = 0;
    FontSize last_size{.width = 0, .height = 0};
    for (const auto& c : text) {
      if (c == '\n') {
        pos_x = 0;
        pos_y += max_height;
      } else if (c == ' ') {
        if (space_width) {
          pos_x += space_width;
        } else {
          HM_ASSIGN_OR_RETURN(last_size, draw(c, surface, imgWidth, imgHeight, pos_x, pos_y, textColor));
          space_width = last_size.width;
          pos_x += last_size.width;
        }
      } else if (c == '\r') {
        pos_x = 0;
      } else {
        HM_ASSIGN_OR_RETURN(last_size, draw(c, surface, imgWidth, imgHeight, pos_x, pos_y, textColor));
        pos_x += last_size.width;
      }
      max_height = std::max(max_height, last_size.height);
    }
    return std::make_pair(pos_x, pos_y);
  }

 protected:
  absl::StatusOr<FontGlyph*> get_or_crerate_glyph(int codepoint) {
    absl::MutexLock lk(&mu_);
    auto found = glyphs_.find(codepoint);
    if (found != glyphs_.end()) {
      return found->second.get();
    }
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
  std::unordered_map<int, std::unique_ptr<FontGlyph>> glyphs_ ABSL_GUARDED_BY(mu_);
  absl::Status status_;
};

class FontCacheImpl : public FontCache {
 public:
  FontCacheImpl() = default;

  absl::StatusOr<std::shared_ptr<Font>> get_or_create_font(
      const std::string& font_path,
      int pixel_height,
      bool lazy_load) override {
    absl::MutexLock lk(&mu_);
    std::pair<int, std::string> key = std::make_pair(pixel_height, font_path);
    auto found = font_cache_.find(key);
    if (found != font_cache_.end()) {
      return found->second;
    }
    found = font_cache_.emplace(std::move(key), std::make_shared<FontImpl>(font_path, pixel_height)).first;
    // It's in there, even if the load fails, since we'll need to not try ot over and over
    if (!lazy_load) {
      HM_RETURN_IF_ERROR(found->second->load());
    }
    return found->second;
  }

 private:
  absl::Mutex mu_;
  std::map<std::pair<int, std::string>, std::shared_ptr<FontImpl>> font_cache_ ABSL_GUARDED_BY(mu_);
};
} // namespace

std::unique_ptr<FontCache> create_font_cache() {
  return std::make_unique<FontCacheImpl>();
}

} // namespace draw_display
} // namespace hm
