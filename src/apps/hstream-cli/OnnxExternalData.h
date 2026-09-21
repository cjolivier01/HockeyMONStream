#pragma once

#include <filesystem>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::pipeline {

// Inspect ONNX protobuf metadata without loading tensors or initializing an
// inference runtime. Returns unique external tensor locations relative to the
// model directory; absolute paths and parent traversal are rejected.
absl::StatusOr<std::vector<std::filesystem::path>> OnnxExternalDataFiles(const std::filesystem::path& model);

} // namespace hm::pipeline
