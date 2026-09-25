#include "hstream/src/libs/draw_display/AnalyticsOverlayInternal.h"

#include <cuda_runtime.h>

namespace hm::draw_display::analytics::detail {
namespace {
__device__ float Clamp(float x) {
  return fminf(1.0F, fmaxf(0.0F, x));
}
__device__ float Coverage(const Command& c, float x, float y, const uint8_t* atlas) {
  if (c.kind == Kind::kLine) {
    const float dx = c.x1 - c.x0, dy = c.y1 - c.y0;
    const float length2 = dx * dx + dy * dy;
    const float u = length2 > 0 ? Clamp(((x - c.x0) * dx + (y - c.y0) * dy) / length2) : 0;
    const float px = x - c.x0 - u * dx, py = y - c.y0 - u * dy;
    return Clamp(c.radius + 0.5F - sqrtf(px * px + py * py));
  }
  if (c.kind == Kind::kDisc) {
    const float dx = x - c.x0, dy = y - c.y0;
    return Clamp(c.radius + 0.5F - sqrtf(dx * dx + dy * dy));
  }
  if (x < c.x0 || x >= c.x1 || y < c.y0 || y >= c.y1)
    return 0;
  if (c.kind == Kind::kRectangle)
    return x < c.x0 + c.radius || x >= c.x1 - c.radius || y < c.y0 + c.radius || y >= c.y1 - c.radius;
  if (c.kind == Kind::kFill)
    return 1;
  if (!atlas)
    return 0;
  // Bilinear atlas sampling confined to this glyph's cell, never a neighbor.
  const float u = (x - c.x0) * kGlyphWidth / (c.x1 - c.x0) - 0.5F;
  const float v = (y - c.y0) * kGlyphHeight / (c.y1 - c.y0) - 0.5F;
  const int ix = static_cast<int>(floorf(u)), iy = static_cast<int>(floorf(v));
  const float fx = u - ix, fy = v - iy;
  const int ox = (c.glyph % kAtlasColumns) * kGlyphWidth, oy = (c.glyph / kAtlasColumns) * kGlyphHeight;
  float coverage = 0;
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i) {
      const int sx = ix + i, sy = iy + j;
      if (sx >= 0 && sx < kGlyphWidth && sy >= 0 && sy < kGlyphHeight)
        coverage += atlas[(oy + sy) * kAtlasWidth + ox + sx] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
    }
  return coverage / 255.0F;
}

template <bool Packed>
__device__ float4 Read(const uint32_t pixel) {
  if constexpr (Packed)
    return make_float4(
        (pixel & 1023U) / 1023.0F,
        ((pixel >> 10) & 1023U) / 1023.0F,
        ((pixel >> 20) & 1023U) / 1023.0F,
        (pixel >> 30) / 3.0F);
  return make_float4(
      (pixel & 255U) / 255.0F, ((pixel >> 8) & 255U) / 255.0F, ((pixel >> 16) & 255U) / 255.0F, (pixel >> 24) / 255.0F);
}
__device__ uint32_t Quantize(float value, unsigned maximum) {
  return static_cast<uint32_t>(floorf(Clamp(value) * maximum + 0.5F));
}
template <bool Packed>
__device__ uint32_t Write(float4 value) {
  if constexpr (Packed)
    return Quantize(value.x, 1023) | (Quantize(value.y, 1023) << 10) | (Quantize(value.z, 1023) << 20) |
        (Quantize(value.w, 3) << 30);
  return Quantize(value.x, 255) | (Quantize(value.y, 255) << 8) | (Quantize(value.z, 255) << 16) |
      (Quantize(value.w, 255) << 24);
}
template <bool Packed>
__global__ void Draw(
    ImageView image,
    const Command* commands,
    const Tile* tiles,
    const uint32_t* references,
    const uint8_t* atlas) {
  const Tile tile = tiles[blockIdx.x];
  const uint32_t x = tile.x * kTileSize + threadIdx.x;
  const uint32_t y = tile.y * kTileSize + threadIdx.y;
  if (x >= image.width || y >= image.height)
    return;
  auto* pixel = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(image.data) + y * image.pitch) + x;
  bool touched = false;
  float4 value{};
  for (uint32_t i = 0; i < tile.reference_count; ++i) {
    const Command c = commands[references[tile.first_reference + i]];
    const float alpha = c.color.alpha * Coverage(c, x + 0.5F, y + 0.5F, atlas);
    if (!(alpha > 0))
      continue;
    if (!touched) {
      value = Read<Packed>(*pixel);
      touched = true;
    }
    // Straight-alpha source-over. Opaque video remains opaque; packed 2-bit
    // alpha is decoded/encoded independently of all three 10-bit channels.
    const float old_alpha = value.w * (1 - alpha);
    const float next_alpha = alpha + old_alpha;
    value.x = (c.color.red * alpha + value.x * old_alpha) / next_alpha;
    value.y = (c.color.green * alpha + value.y * old_alpha) / next_alpha;
    value.z = (c.color.blue * alpha + value.z * old_alpha) / next_alpha;
    value.w = next_alpha;
  }
  if (touched)
    *pixel = Write<Packed>(value);
}
} // namespace

cudaError_t Raster(
    const ImageView& image,
    const Command* commands,
    const Tile* tiles,
    const uint32_t* references,
    uint32_t tile_count,
    const uint8_t* atlas,
    cudaStream_t stream) {
  const dim3 threads(kTileSize, kTileSize);
  if (image.format == PixelFormat::kRgb10A2)
    Draw<true><<<tile_count, threads, 0, stream>>>(image, commands, tiles, references, atlas);
  else
    Draw<false><<<tile_count, threads, 0, stream>>>(image, commands, tiles, references, atlas);
  return cudaGetLastError();
}
} // namespace hm::draw_display::analytics::detail
