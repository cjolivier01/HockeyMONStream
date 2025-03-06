#pragma once

#include <string>

namespace hm {
namespace stitching {

std::pair<double, double> synchronize_by_audio(
    const std::string& file1_path,
    const std::string& file2_path,
    double seconds = 15.0,
    bool verbose = true);

} // namespace stitching
} // namespace hm
