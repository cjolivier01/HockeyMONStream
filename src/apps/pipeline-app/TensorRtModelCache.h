#pragma once

#include <filesystem>

#include "absl/status/status.h"
#include "yaml-cpp/yaml.h"

namespace hm::pipeline {

// DeepStream derives the output engine path from the ONNX path after an
// engine-cache miss. Redirect inference configs backed by read-only packaged
// models through a writable per-user cache before parsing the pipeline.
absl::Status PrepareTensorRtModelCache(YAML::Node pipeline, const std::filesystem::path& config_directory);

} // namespace hm::pipeline
