#pragma once

#include <filesystem>
#include <string>

#include "absl/status/statusor.h"

namespace hm::onnx {
struct ImageInput {
  std::string name;
  int width, height;
};
// Inspect the ONNX graph's input declaration without loading tensor payloads
// or creating an inference session. Requires one float32 NCHW image input,
// static RGB/spatial dimensions, and batch 1 or a symbolic batch dimension.
// Unsupported/ambiguous shapes fail instead of guessing model dimensions.
absl::StatusOr<ImageInput> ReadImageInput(const std::filesystem::path& model);
} // namespace hm::onnx
