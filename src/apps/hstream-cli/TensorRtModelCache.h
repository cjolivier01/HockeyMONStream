#pragma once

#include <filesystem>
#include <functional>

#include "absl/status/status.h"
#include "hstream/src/libs/common/TensorRtGpuIdentity.h"
#include "src/apps/int8-calib-builder/Bf16Engine.h"
#include "yaml-cpp/yaml.h"

namespace hm::pipeline {

// DeepStream derives the output engine path from the ONNX path after an
// engine-cache miss. Redirect YAML ONNX inference configs without an existing
// engine through a persistent per-user cache before parsing the pipeline,
// including models in writable development and downloaded-asset directories.
using GpuNameProvider = std::function<absl::StatusOr<std::string>(unsigned)>;
struct TensorRtModelCacheOptions {
  std::function<bool()> cancelled;
  std::function<void(const std::string&)> progress;
  // Injectable engine operations let cache/selection regressions run without a GPU.
  std::function<absl::Status(const std::filesystem::path&, const hm::inference::Bf16EngineOptions&)> validate_bf16 =
      hm::inference::ValidateBf16Engine;
  std::function<
      absl::Status(const std::filesystem::path&, const std::filesystem::path&, const hm::inference::Bf16EngineOptions&)>
      build_bf16 = hm::inference::BuildBf16Engine;
};
absl::Status PrepareTensorRtModelCache(
    YAML::Node pipeline,
    const std::filesystem::path& config_directory,
    const GpuNameProvider& gpu_name = hm::inference::TensorRtGpuName,
    const TensorRtModelCacheOptions& options = {});

// DeepStream writes engines non-atomically while nvinfer initializes. The
// application keeps these interprocess locks through the pipeline's PAUSED
// transition, then releases them once model initialization is complete.
void ReleaseTensorRtModelCacheLocks();

} // namespace hm::pipeline
