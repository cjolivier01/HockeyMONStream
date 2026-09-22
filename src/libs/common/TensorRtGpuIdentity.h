#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::inference {

// CUDA ordinals honor CUDA_VISIBLE_DEVICES, unlike a shell query of nvidia-smi.
absl::StatusOr<std::string> TensorRtGpuName(unsigned device);
std::string SanitizeGpuName(std::string name);
// Explicit paths stay explicit; bundled paths use {gpu} to select this device.
std::string ResolveGpuEnginePath(std::string path, const std::string& gpu_name);

} // namespace hm::inference
