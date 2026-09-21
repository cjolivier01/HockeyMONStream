#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "absl/status/statusor.h"

namespace hm::pipeline {

// Stable model/config/filter identity. An ONNX-backed detector may create its
// engine during preroll, so its engine bytes are bound separately below.
absl::StatusOr<std::string> PlayerFrameDetectorModelIdentity(
    const std::filesystem::path& config_path,
    uint32_t component_id,
    const std::filesystem::path& engine_override = {});

// Called after the first successful inference with nvinfer's actual engine path,
// then again after shutdown with the same saved path. Missing engines/parsers fail.
absl::StatusOr<std::string> PlayerFrameDetectorRuntimeIdentity(
    const std::filesystem::path& config_path,
    const std::filesystem::path& actual_engine_path);

} // namespace hm::pipeline
