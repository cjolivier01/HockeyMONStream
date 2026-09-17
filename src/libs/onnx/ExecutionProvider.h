#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::onnx {

enum class ExecutionProvider { kCpu, kCuda };
const char* ExecutionProviderName(ExecutionProvider provider);
absl::StatusOr<ExecutionProvider> ParseExecutionProvider(const std::string& value);

} // namespace hm::onnx
