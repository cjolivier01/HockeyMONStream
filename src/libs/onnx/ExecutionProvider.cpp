#include "hstream/src/libs/onnx/ExecutionProvider.h"

#include "absl/status/status.h"

namespace hm::onnx {

const char* ExecutionProviderName(ExecutionProvider provider) {
  return provider == ExecutionProvider::kCuda ? "cuda" : "cpu";
}

absl::StatusOr<ExecutionProvider> ParseExecutionProvider(const std::string& value) {
  if (value == "cuda")
    return ExecutionProvider::kCuda;
  if (value == "cpu")
    return ExecutionProvider::kCpu;
  return absl::InvalidArgumentError("ONNX execution provider must be cuda or cpu, got: " + value);
}

} // namespace hm::onnx
