#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
// X11 defines None as a macro; ORT 1.30 also has OrtResourceCount::None().
#pragma push_macro("None")
#undef None
#include "onnxruntime_cxx_api.h"
#pragma pop_macro("None")
#include "hstream/src/libs/onnx/ExecutionProvider.h"

namespace hm::onnx {

struct TensorContract {
  std::string name;
  ONNXTensorElementDataType type{ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED};
  // A negative dimension accepts any positive runtime size.
  std::vector<int64_t> dimensions;
};

struct FloatInput {
  std::string name;
  std::vector<int64_t> shape;
  const float* data{nullptr};
  size_t element_count{0};
};

struct CpuFallbackOptions {
  // Empty disables fallback. May name a CPU-specific graph with the same I/O
  // contract (SuperPoint), or the original graph (rink segmentation).
  std::string model_path;
  std::function<void()> on_fallback;
};

absl::StatusOr<size_t> checked_element_count(const std::vector<int64_t>& dimensions);
absl::Status validate_shape(const std::vector<int64_t>& actual, const std::vector<int64_t>& expected);

class Tensor {
 public:
  explicit Tensor(Ort::Value value);
  Tensor(Tensor&&) noexcept = default;
  Tensor& operator=(Tensor&&) noexcept = default;
  Tensor(const Tensor&) = delete;
  Tensor& operator=(const Tensor&) = delete;

  ONNXTensorElementDataType type() const;
  std::vector<int64_t> shape() const;
  absl::StatusOr<const float*> float_data() const;
  absl::StatusOr<const int64_t*> int64_data() const;
  absl::StatusOr<size_t> element_count() const;

 private:
  Ort::Value value_;
};

class Session {
 public:
  // Large dynamic models can opt out of retaining temporary buffers in the CPU arena.
  static absl::StatusOr<std::unique_ptr<Session>> Create(
      const std::string& model_path,
      std::vector<TensorContract> inputs,
      std::vector<TensorContract> outputs,
      bool use_cpu_memory_arena = true,
      ExecutionProvider provider = ExecutionProvider::kCpu,
      const std::string& profile_prefix = {},
      const CpuFallbackOptions& cpu_fallback = {});
  static absl::StatusOr<std::unique_ptr<Session>> CreateFromBytes(
      const void* bytes,
      size_t byte_count,
      std::vector<TensorContract> inputs,
      std::vector<TensorContract> outputs);

  absl::StatusOr<std::vector<Tensor>> RunFloat(
      const std::string& input_name,
      const std::vector<int64_t>& input_shape,
      const float* input_data,
      size_t input_count,
      const std::function<bool()>& is_cancelled = {});
  absl::StatusOr<std::vector<Tensor>> RunFloatInputs(
      const std::vector<FloatInput>& inputs,
      const std::function<bool()>& is_cancelled = {});

  ExecutionProvider execution_provider() const {
    return provider_;
  }

 private:
  Session(
      std::unique_ptr<Ort::Session> session,
      std::vector<TensorContract> inputs,
      std::vector<TensorContract> outputs);

  absl::Status ValidateModelContract() const;
  absl::Status FallBackToCpu();

  std::unique_ptr<Ort::Session> session_;
  std::vector<TensorContract> inputs_;
  std::vector<TensorContract> outputs_;
  ExecutionProvider provider_{ExecutionProvider::kCpu};
  bool use_cpu_memory_arena_{true};
  CpuFallbackOptions cpu_fallback_;
  absl::Status fallback_failure_;
};

} // namespace hm::onnx
