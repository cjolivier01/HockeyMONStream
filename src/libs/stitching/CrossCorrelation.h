#pragma once

#include <vector>

namespace hm {
namespace stitching {

std::vector<float> full_correlate(const std::vector<float>& x, const std::vector<float>& y);

std::vector<float> full_correlate_fft(const std::vector<float>& x, const std::vector<float>& y);

} // namespace stitching
} // namespace hm
