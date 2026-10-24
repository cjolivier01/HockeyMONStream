#pragma once

#include <cstddef>
#include <cstdint>

namespace hm::gpu {

enum class PixelFormat { kRgba8, kRgb10A2, kNv12, kP010, kRgb8, kGray8, kRgba16Float };

// Matrix weights and legal-range scaling in native 8/10-bit code units.
struct YuvEncoding {
  float kr, kb, y_offset, y_range, uv_offset, uv_range;
};

// Borrowed GPU planes. YUV inputs require explicit range/matrix coefficients:
// rgb = y_scale * (Y - y_offset) + chroma coefficients * (U/V - uv_offset).
// Components are in 8-bit video units, including P010 scaled by 255/1023.
// RGBA16Float stores four IEEE binary16 components in the same 0..255 units
// used by the panorama engine (not normalized 0..1).
struct Image {
  const void* plane0;
  const void* plane1;
  size_t pitch0, pitch1;
  int width, height;
  PixelFormat format;
  float y_offset, uv_offset, y_scale, r_v, g_u, g_v, b_u;
};

// Validate plane dimensions, pitches, alignment, address extents and YUV coefficients.
// Allocation ownership and backing size remain the caller's responsibility.
bool ValidateImage(const Image& image);

} // namespace hm::gpu
