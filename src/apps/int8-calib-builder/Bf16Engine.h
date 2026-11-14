#pragma once

#include <filesystem>
#include <functional>
#include <optional>
#include <string>

#include "absl/status/status.h"

namespace hm::inference {

struct Bf16EngineOptions {
  unsigned gpu;
  unsigned batch;
  // An absent override retains TensorRT's workspace policy, as nvinfer does.
  std::optional<unsigned> workspace_mib;
  std::function<bool()> cancelled;
  std::function<void(const std::string&)> progress;
};

// Both operations use the SDK matched to DeepStream. Validation includes actual
// BF16 execution, FP32 parser boundaries and the requested runtime batch.
absl::Status ValidateBf16Engine(const std::filesystem::path& engine, const Bf16EngineOptions& options);
// Publish only after building, deserializing and inspecting the complete engine.
absl::Status BuildBf16Engine(
    const std::filesystem::path& onnx,
    const std::filesystem::path& engine,
    const Bf16EngineOptions& options);

} // namespace hm::inference
