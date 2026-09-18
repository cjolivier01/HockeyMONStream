#pragma once

#include <string>

#include "absl/status/statusor.h"

namespace hm::onnx {

enum class ExecutionProvider { kCpu, kCuda };
const char* ExecutionProviderName(ExecutionProvider provider);
absl::StatusOr<ExecutionProvider> ParseExecutionProvider(const std::string& value);
// ORT's arena error omits the device. It is unambiguously a GPU allocation
// only when CUDA is active and the CPU arena has been disabled.
bool IsCudaOutOfMemory(const std::string& message, ExecutionProvider provider, bool use_cpu_memory_arena);

} // namespace hm::onnx
