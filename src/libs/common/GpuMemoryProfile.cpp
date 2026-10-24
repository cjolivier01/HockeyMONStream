#include "GpuMemoryProfile.h"

#include <string>

#include "hstream/src/libs/gpu/Runtime.h"

namespace hm::inference {
absl::StatusOr<uint64_t> GpuTotalMemoryBytes(unsigned device) {
  cudaDeviceProp properties{};
  const auto status = cudaGetDeviceProperties(&properties, device);
  if (status != cudaSuccess)
    return absl::FailedPreconditionError(
        "Cannot query GPU " + std::to_string(device) + ": " + cudaGetErrorString(status));
  return static_cast<uint64_t>(properties.totalGlobalMem);
}

bool UseLowMemoryProfile(uint64_t total_memory_bytes) {
  return total_memory_bytes <= 8ULL * 1024ULL * 1024ULL * 1024ULL;
}
} // namespace hm::inference
