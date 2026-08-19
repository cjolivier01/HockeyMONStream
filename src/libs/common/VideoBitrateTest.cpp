#include "hstream/src/libs/common/VideoBitrate.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  bool ok = true;

  const auto fractional_bitrate = hm::make_bitrate_per_pixel(/*bitrate=*/7, /*width=*/3, /*height=*/2);
  const auto fractional_scaled =
      fractional_bitrate ? hm::scale_bitrate(*fractional_bitrate, /*width=*/5, /*height=*/2) : std::nullopt;
  ok &= expect(
      fractional_bitrate.has_value() && fractional_bitrate->numerator == 7 && fractional_bitrate->denominator == 6 &&
          fractional_scaled == 12,
      "Bitrate scaling must preserve a reduced bits-per-pixel fraction and round only the final result");

  constexpr uint64_t kSourceBitrate = 2000000001;
  constexpr uint64_t kSourceWidth = 5312;
  constexpr uint64_t kSourceHeight = 2988;
  constexpr uint64_t kDestinationWidth = 7680;
  constexpr uint64_t kDestinationHeight = 4320;
  const auto precise_bitrate = hm::make_bitrate_per_pixel(kSourceBitrate, kSourceWidth, kSourceHeight);
  const auto source_sized_bitrate =
      precise_bitrate ? hm::scale_bitrate(*precise_bitrate, kSourceWidth, kSourceHeight) : std::nullopt;
  const auto destination_bitrate =
      precise_bitrate ? hm::scale_bitrate(*precise_bitrate, kDestinationWidth, kDestinationHeight) : std::nullopt;
  const uint64_t expected_destination_bitrate =
      (kSourceBitrate * kDestinationWidth * kDestinationHeight + kSourceWidth * kSourceHeight / 2) /
      (kSourceWidth * kSourceHeight);
  ok &= expect(
      source_sized_bitrate == kSourceBitrate && destination_bitrate == expected_destination_bitrate,
      "Large resolution scaling must use exact integer arithmetic without intermediate floating-point precision loss");

  ok &= expect(
      !hm::make_bitrate_per_pixel(0, kSourceWidth, kSourceHeight).has_value() &&
          !hm::make_bitrate_per_pixel(1, std::numeric_limits<uint64_t>::max(), 2).has_value() &&
          !hm::scale_bitrate(hm::BitratePerPixel{1, 0}, kDestinationWidth, kDestinationHeight).has_value() &&
          !hm::scale_bitrate(hm::BitratePerPixel{1, 1}, std::numeric_limits<uint64_t>::max(), 2).has_value(),
      "Invalid bitrate fractions must be rejected");

  return ok ? 0 : 1;
}
