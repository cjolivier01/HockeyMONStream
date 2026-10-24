#pragma once

#include "hstream/src/libs/gpu/Image.h"
#include "hstream/src/libs/gpu/Runtime.h"

namespace hm::gpu {
__device__ inline float Clamp(float value, float low, float high) {
  return fminf(fmaxf(value, low), high);
}
__device__ inline float4 Pixel(const Image& image, int x, int y) {
  const auto* row = static_cast<const uint8_t*>(image.plane0) + static_cast<size_t>(y) * image.pitch0;
  if (image.format == PixelFormat::kRgba8) {
    const auto p = reinterpret_cast<const uchar4*>(row)[x];
    return make_float4(p.x, p.y, p.z, p.w);
  }
  if (image.format == PixelFormat::kRgb10A2) {
    const auto p = reinterpret_cast<const uint32_t*>(row)[x];
    return make_float4(
        (p & 1023) * (255.f / 1023),
        ((p >> 10) & 1023) * (255.f / 1023),
        ((p >> 20) & 1023) * (255.f / 1023),
        (p >> 30) * 85.f);
  }
  if (image.format == PixelFormat::kRgb8) {
    const auto* p = row + static_cast<size_t>(x) * 3;
    return make_float4(p[0], p[1], p[2], 255);
  }
  if (image.format == PixelFormat::kGray8)
    return make_float4(row[x], row[x], row[x], 255);
  const auto* chroma = static_cast<const uint8_t*>(image.plane1) + static_cast<size_t>(y / 2) * image.pitch1;
  float luma, u, v;
  if (image.format == PixelFormat::kNv12) {
    luma = row[x];
    u = chroma[(x / 2) * 2];
    v = chroma[(x / 2) * 2 + 1];
  } else {
    luma = (reinterpret_cast<const uint16_t*>(row)[x] >> 6) * (255.f / 1023);
    u = (reinterpret_cast<const uint16_t*>(chroma)[(x / 2) * 2] >> 6) * (255.f / 1023);
    v = (reinterpret_cast<const uint16_t*>(chroma)[(x / 2) * 2 + 1] >> 6) * (255.f / 1023);
  }
  luma = (luma - image.y_offset) * image.y_scale;
  u -= image.uv_offset;
  v -= image.uv_offset;
  return make_float4(
      Clamp(luma + image.r_v * v, 0, 255),
      Clamp(luma + image.g_u * u + image.g_v * v, 0, 255),
      Clamp(luma + image.b_u * u, 0, 255),
      255);
}
__device__ inline float Mix(float a, float b, float c, float d, float dx, float dy) {
  return (a * (1 - dx) + b * dx) * (1 - dy) + (c * (1 - dx) + d * dx) * dy;
}
} // namespace hm::gpu
