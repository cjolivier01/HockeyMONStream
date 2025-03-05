#pragma once

#include <string>

namespace hm {
namespace stiching {

std::pair<int, int> synchronize_by_audio(
    const std::string& file1_path,
    const std::string& file2_path,
    double seconds = 15.0,
    bool verbose = true);
}
} // namespace hm
