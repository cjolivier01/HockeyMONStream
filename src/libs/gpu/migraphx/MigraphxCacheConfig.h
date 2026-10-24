#pragma once

#include <cstddef>
#include <filesystem>

namespace hm::gpu {
struct MigraphxCacheConfig {
  std::filesystem::path directory;
  size_t maximum_entry_bytes;
};
} // namespace hm::gpu
