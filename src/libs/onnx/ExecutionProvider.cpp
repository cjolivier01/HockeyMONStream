#include "hstream/src/libs/onnx/ExecutionProvider.h"

#include <algorithm>
#include <cctype>
#include <regex>

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

bool IsCudaOutOfMemory(const std::string& message, ExecutionProvider provider, bool use_cpu_memory_arena) {
  if (provider != ExecutionProvider::kCuda)
    return false;
  std::string lower = message;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
  static const std::regex cuda_allocation_status(R"(cuda failure:?\s+(?:status=)?2(?:\D|$))");
  return lower.find("cudaerrormemoryallocation") != std::string::npos ||
      lower.find("cuda_error_out_of_memory") != std::string::npos ||
      lower.find("cudnn_status_alloc_failed") != std::string::npos ||
      std::regex_search(lower, cuda_allocation_status) ||
      (!use_cpu_memory_arena && lower.find("bfcarena") != std::string::npos &&
       lower.find("failed to allocate memory for requested buffer") != std::string::npos);
}

} // namespace hm::onnx
