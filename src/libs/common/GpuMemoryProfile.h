#pragma once

#include <cstdint>

#include "absl/status/statusor.h"

namespace hm::inference {
// Query the build's selected CUDA/HIP runtime, honoring its visible devices.
absl::StatusOr<uint64_t> GpuTotalMemoryBytes(unsigned device);
bool UseLowMemoryProfile(uint64_t total_memory_bytes);
} // namespace hm::inference
