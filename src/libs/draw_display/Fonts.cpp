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
    return FontSize {
      .width = glyphWidth_, .height = glyphHeight_;
    }
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

class Font {
 public:
  Font(const std::string& font_path, int pixel_height) : font_path_(font_path), pixel_height_(pixel_height) {
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
      uchar4 textColor) {
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
    HM_ASSIGN_OR_RETURN(g, get_or_crerate_glyph(codepoint));
    cudaError_t cerr = g->draw(surface, imgWidth, imgHeight, dest_x, dest_y, textColor);
    if (cerr != cudaError_t::cudaSuccess) {
      return to_status(cerr);
    }
    return g->size();
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

class FontCache {
 public:
  FontCache() = default;

  // absl

 private:
  std::map<std::pair<int, std::string>, CudaBuffer> font_cache_;
};

//------------------------------------------------------------------------------
// Host Code
//------------------------------------------------------------------------------
// This example does the following:
// 1. Loads a TrueType font file into memory.
// 2. Uses stb_truetype to initialize the font and render the glyph for 'A'
//    at a desired pixel height.
// 3. Copies the glyph bitmap to device memory.
// 4. Creates a dummy destination RGBA image on the GPU.
// 5. Launches the CUDA kernel to overlay the glyph onto the image.
// 6. Copies the image back to host memory and writes it out as a PPM file.
int test_main() {
  //--------------------------------------------------------------------------
  // 1. Load the TrueType font file.
  //--------------------------------------------------------------------------
  const char* fontFilePath = "path/to/font.ttf"; // <-- Update with your font file path.
  std::ifstream fontFile(fontFilePath, std::ios::binary);
  if (!fontFile) {
    std::cerr << "Error: Could not open font file: " << fontFilePath << std::endl;
    return 1;
  }
  std::vector<unsigned char> fontBuffer((std::istreambuf_iterator<char>(fontFile)), std::istreambuf_iterator<char>());
  fontFile.close();

  //--------------------------------------------------------------------------
  // 2. Initialize stb_truetype and render a glyph.
  //--------------------------------------------------------------------------
  stbtt_fontinfo font;
  if (!stbtt_InitFont(&font, fontBuffer.data(), 0)) {
    std::cerr << "Error: Could not initialize font." << std::endl;
    return 1;
  }

  // Set the desired pixel height (for example, 48 pixels tall).
  int pixelHeight = 48;
  float scale = stbtt_ScaleForPixelHeight(&font, pixelHeight);

  // Choose a codepoint to extract; here we render 'A'.
  int codepoint = 'A';
  int glyphWidth, glyphHeight, xoffset, yoffset;
  unsigned char* glyphBitmap =
      stbtt_GetCodepointBitmap(&font, 0, scale, codepoint, &glyphWidth, &glyphHeight, &xoffset, &yoffset);
  if (!glyphBitmap) {
    std::cerr << "Error: Could not generate bitmap for character '" << char(codepoint) << "'." << std::endl;
    return 1;
  }
  std::cout << "Generated glyph bitmap for '" << char(codepoint) << "' with dimensions: " << glyphWidth << " x "
            << glyphHeight << std::endl;

  //--------------------------------------------------------------------------
  // 3. Copy the glyph bitmap to GPU memory.
  //--------------------------------------------------------------------------
  unsigned char* d_glyph;
  size_t glyphSize = glyphWidth * glyphHeight * sizeof(unsigned char);
  cudaError_t err = cudaMalloc(&d_glyph, glyphSize);
  if (err != cudaSuccess) {
    std::cerr << "CUDA Error (cudaMalloc for glyph): " << cudaGetErrorString(err) << std::endl;
    stbtt_FreeBitmap(glyphBitmap, nullptr);
    return 1;
  }
  cudaMemcpy(d_glyph, glyphBitmap, glyphSize, cudaMemcpyHostToDevice);
  // Free the glyph bitmap on the host now that it's on the GPU.
  stbtt_FreeBitmap(glyphBitmap, nullptr);

  //--------------------------------------------------------------------------
  // 4. Create a destination image on the GPU.
  //--------------------------------------------------------------------------
  // For this example, we create an 800x600 RGBA image.
  int imgWidth = 800;
  int imgHeight = 600;
  size_t imgSize = imgWidth * imgHeight * sizeof(uchar4);
  uchar4* d_img;
  err = cudaMalloc(&d_img, imgSize);
  if (err != cudaSuccess) {
    std::cerr << "CUDA Error (cudaMalloc for image): " << cudaGetErrorString(err) << std::endl;
    cudaFree(d_glyph);
    return 1;
  }
  // Clear the image to black.
  cudaMemset(d_img, 0, imgSize);

  //--------------------------------------------------------------------------
  // 5. Launch the CUDA kernel to draw the glyph onto the image.
  //--------------------------------------------------------------------------
  // Specify the position where the glyph should be drawn.
  int destX = 100;
  int destY = 100;

  // Set the text color (white in this example).
  uchar4 textColor;
  textColor.x = 255; // R
  textColor.y = 255; // G
  textColor.z = 255; // B
  textColor.w = 255; // A

  // Threshold to decide when to draw a glyph pixel.
  // For an 8-bit grayscale bitmap, any nonzero value will be drawn.
  float threshold = 1.0f;

  // Define CUDA kernel launch dimensions based on the glyph size.
  drawGlyph(
      d_img,
      imgWidth,
      imgHeight,
      d_glyph,
      glyphWidth,
      glyphHeight,
      destX,
      destY,
      textColor,
      threshold,
      /*stream=*/nullptr);
  cudaDeviceSynchronize();

  //--------------------------------------------------------------------------
  // 6. Copy the resulting image back to the host and write it out.
  //--------------------------------------------------------------------------
  std::vector<uchar4> h_img(imgWidth * imgHeight);
  cudaMemcpy(h_img.data(), d_img, imgSize, cudaMemcpyDeviceToHost);

  // Save the image as a PPM file (ignoring the alpha channel).
  std::ofstream outFile("output.ppm", std::ios::binary);
  if (!outFile) {
    std::cerr << "Error: Could not open output file for writing." << std::endl;
    cudaFree(d_glyph);
    cudaFree(d_img);
    return 1;
  }
  outFile << "P6\n" << imgWidth << " " << imgHeight << "\n255\n";
  for (int i = 0; i < imgWidth * imgHeight; i++) {
    unsigned char r = h_img[i].x;
    unsigned char g = h_img[i].y;
    unsigned char b = h_img[i].z;
    outFile.write(reinterpret_cast<char*>(&r), 1);
    outFile.write(reinterpret_cast<char*>(&g), 1);
    outFile.write(reinterpret_cast<char*>(&b), 1);
  }
  outFile.close();
  std::cout << "Output image saved as output.ppm" << std::endl;

  //--------------------------------------------------------------------------
  // Cleanup
  //--------------------------------------------------------------------------
  cudaFree(d_glyph);
  cudaFree(d_img);
  return 0;
}

} // namespace draw_display
} // namespace hm
