#include "TensorRtGpuIdentity.h"

#include <cuda_runtime.h>

namespace hm::inference {

namespace {

absl::StatusOr<cudaDeviceProp> CudaGpuProperties(unsigned device) {
  cudaDeviceProp properties{};
  const auto status = cudaGetDeviceProperties(&properties, device);
  if (status != cudaSuccess)
    return absl::FailedPreconditionError(
        "Cannot identify CUDA GPU " + std::to_string(device) + ": " + cudaGetErrorString(status));
  return properties;
}

} // namespace

std::string SanitizeGpuName(std::string name) {
  for (char& c : name) {
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')))
      c = '_';
  }
  return name;
}

absl::StatusOr<std::string> TensorRtGpuName(unsigned device) {
  auto properties = CudaGpuProperties(device);
  if (!properties.ok())
    return properties.status();
  return SanitizeGpuName(properties->name);
}

absl::StatusOr<uint64_t> CudaGpuTotalMemoryBytes(unsigned device) {
  return GpuTotalMemoryBytes(device);
}

std::string ResolveGpuEnginePath(std::string path, const std::string& gpu_name) {
  size_t position = 0;
  while ((position = path.find("{gpu}", position)) != std::string::npos) {
    path.replace(position, 5, gpu_name);
    position += gpu_name.size();
  }
  return path;
}

} // namespace hm::inference
