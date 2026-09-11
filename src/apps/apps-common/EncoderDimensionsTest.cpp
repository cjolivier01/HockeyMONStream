#include "hstream/src/apps/apps-common/EncoderDimensions.h"

#include <cstdlib>
#include <iostream>

int main() {
  const hm::EncoderDimensionLimits hevc{130, 34, 8192, 8192, 262144};
  auto check = [&](guint width,
                   guint height,
                   guint expected_width,
                   guint expected_height,
                   const hm::EncoderDimensionLimits& limits) {
    auto size = hm::fit_encoder_dimensions(width, height, limits);
    if (!size || *size != std::make_pair(expected_width, expected_height)) {
      std::cerr << "Unexpected encoder size for " << width << "x" << height << '\n';
      std::exit(1);
    }
  };
  check(7680, 4320, 7680, 4320, hevc);
  check(10000, 3000, 8192, 2456, hevc);
  check(3000, 10000, 2456, 8192, hevc);
  check(8193, 4097, 8192, 4096, hevc);
  check(1921, 1081, 1920, 1080, hevc);
  check(10000, 3000, 7680, 2304, {180, 180, 7680, 7680});
  check(10000, 3000, 4096, 1228, {48, 48, 4096, 4096});
  check(8192, 8192, 4096, 4096, {2, 2, 8192, 8192, 65536});
  for (auto input : {std::make_pair(0U, 100U), std::make_pair(100U, 0U), std::make_pair(100000U, 100U)}) {
    if (hm::fit_encoder_dimensions(input.first, input.second, hevc)) {
      std::cerr << "Unsupported canvas should fail rather than upscale or distort\n";
      return 1;
    }
  }
  std::cout << "Encoder dimension fitting checks passed\n";
}
