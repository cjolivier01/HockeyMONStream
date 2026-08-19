#pragma once

#include <cstdint>
#include <limits>
#include <numeric>
#include <optional>

namespace hm {

struct BitratePerPixel {
  uint64_t numerator{0};
  uint64_t denominator{0};

  bool valid() const {
    return numerator > 0 && denominator > 0;
  }
};

inline std::optional<BitratePerPixel> make_bitrate_per_pixel(
    uint64_t bitrate,
    uint64_t source_width,
    uint64_t source_height) {
  if (bitrate == 0 || source_width == 0 || source_height == 0) {
    return std::nullopt;
  }

  const unsigned __int128 source_pixels_wide =
      static_cast<unsigned __int128>(source_width) * static_cast<unsigned __int128>(source_height);
  if (source_pixels_wide > std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }

  const uint64_t source_pixels = static_cast<uint64_t>(source_pixels_wide);
  const uint64_t divisor = std::gcd(bitrate, source_pixels);
  return BitratePerPixel{bitrate / divisor, source_pixels / divisor};
}

inline std::optional<uint64_t> scale_bitrate(
    const BitratePerPixel& bitrate_per_pixel,
    uint64_t destination_width,
    uint64_t destination_height) {
  if (!bitrate_per_pixel.valid() || destination_width == 0 || destination_height == 0) {
    return std::nullopt;
  }

  const unsigned __int128 destination_pixels =
      static_cast<unsigned __int128>(destination_width) * static_cast<unsigned __int128>(destination_height);
  if (destination_pixels > std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  const unsigned __int128 scaled_numerator =
      static_cast<unsigned __int128>(bitrate_per_pixel.numerator) * static_cast<uint64_t>(destination_pixels);
  const unsigned __int128 rounded_bitrate =
      (scaled_numerator + bitrate_per_pixel.denominator / 2) / bitrate_per_pixel.denominator;
  if (rounded_bitrate > std::numeric_limits<uint64_t>::max()) {
    return std::nullopt;
  }
  return static_cast<uint64_t>(rounded_bitrate);
}

} // namespace hm
