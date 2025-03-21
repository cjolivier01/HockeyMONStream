#include <cuda_runtime.h>

namespace hm {
namespace draw_display {
namespace {
//------------------------------------------------------------------------------
// CUDA Kernel: drawGlyphKernel
//------------------------------------------------------------------------------
// This kernel overlays a glyph (provided as an 8-bit grayscale bitmap)
// onto an RGBA image. For each pixel in the glyph, if the glyph value is
// above a threshold (i.e. non-transparent), the corresponding pixel in the
// destination image is set to the given text color.
__global__ void drawGlyphKernel(
    uchar4* d_img,
    int imgWidth,
    int imgHeight,
    const unsigned char* d_glyph,
    int glyphWidth,
    int glyphHeight,
    int destX,
    int destY,
    uchar4 textColor,
    float threshold) {
  // Compute the pixel location within the glyph.
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= glyphWidth || y >= glyphHeight)
    return;

  // Get the glyph's pixel value.
  int glyphIndex = y * glyphWidth + x;
  unsigned char pixelValue = d_glyph[glyphIndex];

  // If the pixel is "on" (above threshold), write the text color to the image.
  if (pixelValue > threshold) {
    int destPixelX = destX + x;
    int destPixelY = destY + y;
    if (destPixelX >= 0 && destPixelX < imgWidth && destPixelY >= 0 && destPixelY < imgHeight) {
      int imgIndex = destPixelY * imgWidth + destPixelX;
      d_img[imgIndex] = textColor;
    }
  }
}
} // namespace

cudaError_t drawGlyph(
    uchar4* d_img,
    int imgWidth,
    int imgHeight,
    const unsigned char* d_glyph,
    int glyphWidth,
    int glyphHeight,
    int destX,
    int destY,
    uchar4 textColor,
    float threshold,
    cudaStream_t stream) {
  dim3 block(16, 16);
  dim3 grid((glyphWidth + block.x - 1) / block.x, (glyphHeight + block.y - 1) / block.y);
  drawGlyphKernel<<<grid, block, 0, stream>>>(
      d_img, imgWidth, imgHeight, d_glyph, glyphWidth, glyphHeight, destX, destY, textColor, threshold);
  return cudaGetLastError();
}
} // namespace draw_display
} // namespace hm
