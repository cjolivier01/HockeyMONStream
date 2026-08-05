#pragma once

#include <filesystem>

#include "absl/status/status.h"
#include "yaml-cpp/yaml.h"

namespace hm::pipeline {

// DeepStream derives the output engine path from the ONNX path after an
// engine-cache miss. Redirect inference configs backed by read-only packaged
// models through a writable per-user cache before parsing the pipeline.
absl::Status PrepareTensorRtModelCache(YAML::Node pipeline, const std::filesystem::path& config_directory);

// DeepStream writes engines non-atomically while nvinfer initializes. The
// application keeps these interprocess locks through the pipeline's PAUSED
// transition, then releases them once model initialization is complete.
void ReleaseTensorRtModelCacheLocks();

} // namespace hm::pipeline
