#pragma once
#include <filesystem>
#include "absl/status/status.h"
class HmApp;
namespace hm {
absl::Status ConfigureOutputRinkMap(HmApp& app, const std::filesystem::path& game, bool preparation);
}
