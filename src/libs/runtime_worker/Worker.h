#pragma once
#include <filesystem>
#include "hstream/src/libs/runtime_worker/Json.h"

namespace hm::runtime_worker {
Json Capabilities();
int Run(const std::filesystem::path& request);
} // namespace hm::runtime_worker
