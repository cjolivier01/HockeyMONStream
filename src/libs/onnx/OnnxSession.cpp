#include "hstream/src/libs/onnx/OnnxSession.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <limits>
#include <sstream>
#include <thread>
#include <utility>

namespace hm::onnx {
namespace {

Ort::Env& environment() {
  static Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "hstream");
  return env;
}

Ort::SessionOptions session_options() {
  Ort::SessionOptions options;
  options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  // Avoid a second unbounded thread pool per session. ONNX Runtime still uses
  // its CPU provider, but calibration remains a predictable background task.
  options.SetIntraOpNumThreads(1);
  options.SetInterOpNumThreads(1);
  return options;
}

std::string shape_string(const std::vector<int64_t>& shape) {
  std::ostringstream out;
  out << '[';
  for (size_t i = 0; i < shape.size(); ++i) {
    if (i != 0)
      out << ',';
    out << shape[i];
  }
  return out.str() + ']';
}

absl::Status ort_error(const std::string& operation, const Ort::Exception& error) {
  return absl::InternalError(operation + ": " + error.what());
}

class RunCancellationMonitor {
 public:
  RunCancellationMonitor(Ort::RunOptions* options, std::function<bool()> is_cancelled)
      : options_(options), is_cancelled_(std::move(is_cancelled)) {
    if (!options_ || !is_cancelled_) {
      return;
    }
    thread_ = std::thread([this] {
      while (!finished_.load(std::memory_order_acquire)) {
        if (is_cancelled_()) {
          try {
            options_->SetTerminate();
          } catch (const Ort::Exception&) {
          }
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
    });
  }

  ~RunCancellationMonitor() {
    finished_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  RunCancellationMonitor(const RunCancellationMonitor&) = delete;
  RunCancellationMonitor& operator=(const RunCancellationMonitor&) = delete;

 private:
  Ort::RunOptions* options_{nullptr};
  std::function<bool()> is_cancelled_;
  std::atomic<bool> finished_{false};
  std::thread thread_;
};

} // namespace

absl::StatusOr<size_t> checked_element_count(const std::vector<int64_t>& dimensions) {
  size_t count = 1;
  for (int64_t dimension : dimensions) {
    if (dimension < 0) {
      return absl::InvalidArgumentError("Concrete tensor dimensions must not be negative: " + shape_string(dimensions));
    }
    if (dimension == 0)
      return 0;
    const auto value = static_cast<size_t>(dimension);
    if (value > std::numeric_limits<size_t>::max() / count) {
      return absl::OutOfRangeError("Tensor element count overflows size_t: " + shape_string(dimensions));
    }
    count *= value;
  }
  return count;
}

absl::Status validate_shape(const std::vector<int64_t>& actual, const std::vector<int64_t>& expected) {
  if (actual.size() != expected.size()) {
    return absl::InvalidArgumentError(
        "Tensor rank mismatch: got " + shape_string(actual) + ", expected " + shape_string(expected));
  }
  for (size_t i = 0; i < actual.size(); ++i) {
    if (actual[i] < 0 || (expected[i] >= 0 && actual[i] != expected[i])) {
      return absl::InvalidArgumentError(
          "Tensor shape mismatch: got " + shape_string(actual) + ", expected " + shape_string(expected));
    }
  }
  return absl::OkStatus();
}

Tensor::Tensor(Ort::Value value) : value_(std::move(value)) {}

ONNXTensorElementDataType Tensor::type() const {
  return value_.GetTensorTypeAndShapeInfo().GetElementType();
}

std::vector<int64_t> Tensor::shape() const {
  return value_.GetTensorTypeAndShapeInfo().GetShape();
}

absl::StatusOr<const float*> Tensor::float_data() const {
  if (type() != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
    return absl::FailedPreconditionError("Tensor does not contain float32 data");
  }
  return value_.GetTensorData<float>();
}

absl::StatusOr<const int64_t*> Tensor::int64_data() const {
  if (type() != ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64) {
    return absl::FailedPreconditionError("Tensor does not contain int64 data");
  }
  return value_.GetTensorData<int64_t>();
}

absl::StatusOr<size_t> Tensor::element_count() const {
  return checked_element_count(shape());
}

Session::Session(
    std::unique_ptr<Ort::Session> session,
    std::vector<TensorContract> inputs,
    std::vector<TensorContract> outputs)
    : session_(std::move(session)), inputs_(std::move(inputs)), outputs_(std::move(outputs)) {}

absl::StatusOr<std::unique_ptr<Session>> Session::Create(
    const std::string& model_path,
    std::vector<TensorContract> inputs,
    std::vector<TensorContract> outputs) {
  try {
    auto options = session_options();
    auto session = std::make_unique<Ort::Session>(environment(), model_path.c_str(), options);
    auto result = std::unique_ptr<Session>(new Session(std::move(session), std::move(inputs), std::move(outputs)));
    auto status = result->ValidateModelContract();
    if (!status.ok())
      return status;
    return result;
  } catch (const Ort::Exception& error) {
    return ort_error("Failed to load ONNX model " + model_path, error);
  }
}

absl::StatusOr<std::unique_ptr<Session>> Session::CreateFromBytes(
    const void* bytes,
    size_t byte_count,
    std::vector<TensorContract> inputs,
    std::vector<TensorContract> outputs) {
  if (bytes == nullptr || byte_count == 0) {
    return absl::InvalidArgumentError("ONNX model bytes must not be empty");
  }
  try {
    auto options = session_options();
    auto session = std::make_unique<Ort::Session>(environment(), bytes, byte_count, options);
    auto result = std::unique_ptr<Session>(new Session(std::move(session), std::move(inputs), std::move(outputs)));
    auto status = result->ValidateModelContract();
    if (!status.ok())
      return status;
    return result;
  } catch (const Ort::Exception& error) {
    return ort_error("Failed to load in-memory ONNX model", error);
  }
}

absl::Status Session::ValidateModelContract() const {
  try {
    if (session_->GetInputCount() != inputs_.size() || session_->GetOutputCount() != outputs_.size()) {
      return absl::FailedPreconditionError("ONNX model input/output count does not match the frozen contract");
    }
    Ort::AllocatorWithDefaultOptions allocator;
    for (size_t i = 0; i < inputs_.size(); ++i) {
      auto name = session_->GetInputNameAllocated(i, allocator);
      auto type_info = session_->GetInputTypeInfo(i);
      auto info = type_info.GetTensorTypeAndShapeInfo();
      if (inputs_[i].name != name.get() || inputs_[i].type != info.GetElementType()) {
        return absl::FailedPreconditionError(
            "ONNX input " + std::to_string(i) + " violates its frozen contract: got name=" + name.get() +
            " type=" + std::to_string(info.GetElementType()) + ", expected name=" + inputs_[i].name +
            " type=" + std::to_string(inputs_[i].type));
      }
      auto status = validate_shape(info.GetShape(), inputs_[i].dimensions);
      // Symbolic model dimensions are reported as -1. Validate the fixed axes
      // here and defer dynamic positive sizes to RunFloat.
      if (!status.ok()) {
        const auto shape = info.GetShape();
        if (shape.size() != inputs_[i].dimensions.size())
          return status;
        for (size_t d = 0; d < shape.size(); ++d) {
          if (inputs_[i].dimensions[d] >= 0 && shape[d] >= 0 && shape[d] != inputs_[i].dimensions[d])
            return status;
        }
      }
    }
    for (size_t i = 0; i < outputs_.size(); ++i) {
      auto name = session_->GetOutputNameAllocated(i, allocator);
      auto type_info = session_->GetOutputTypeInfo(i);
      auto info = type_info.GetTensorTypeAndShapeInfo();
      if (outputs_[i].name != name.get() || outputs_[i].type != info.GetElementType()) {
        return absl::FailedPreconditionError(
            "ONNX output " + std::to_string(i) + " violates its frozen contract: got name=" + name.get() +
            " type=" + std::to_string(info.GetElementType()) + ", expected name=" + outputs_[i].name +
            " type=" + std::to_string(outputs_[i].type));
      }
      const auto actual = info.GetShape();
      if (actual.size() != outputs_[i].dimensions.size()) {
        return absl::FailedPreconditionError(
            "ONNX output " + std::to_string(i) + " rank violates its frozen contract: got " + shape_string(actual) +
            ", expected " + shape_string(outputs_[i].dimensions));
      }
      for (size_t d = 0; d < actual.size(); ++d) {
        if (outputs_[i].dimensions[d] >= 0 && actual[d] >= 0 && actual[d] != outputs_[i].dimensions[d]) {
          return absl::FailedPreconditionError(
              "ONNX output " + std::to_string(i) + " shape violates its frozen contract: got " + shape_string(actual) +
              ", expected " + shape_string(outputs_[i].dimensions));
        }
      }
    }
    return absl::OkStatus();
  } catch (const Ort::Exception& error) {
    return ort_error("Failed to inspect ONNX model", error);
  }
}

absl::StatusOr<std::vector<Tensor>> Session::RunFloat(
    const std::string& input_name,
    const std::vector<int64_t>& input_shape,
    const float* input_data,
    size_t input_count,
    const std::function<bool()>& is_cancelled) const {
  return RunFloatInputs({{input_name, input_shape, input_data, input_count}}, is_cancelled);
}

absl::StatusOr<std::vector<Tensor>> Session::RunFloatInputs(
    const std::vector<FloatInput>& inputs,
    const std::function<bool()>& is_cancelled) const {
  if (is_cancelled && is_cancelled()) {
    return absl::CancelledError("ONNX inference cancelled before execution");
  }
  if (inputs.size() != inputs_.size()) {
    return absl::InvalidArgumentError("Float input count does not match the model contract");
  }
  for (size_t i = 0; i < inputs.size(); ++i) {
    if (inputs_[i].name != inputs[i].name || inputs_[i].type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT) {
      return absl::InvalidArgumentError("Float input does not match the model contract");
    }
    auto status = validate_shape(inputs[i].shape, inputs_[i].dimensions);
    if (!status.ok())
      return status;
    if (std::any_of(inputs[i].shape.begin(), inputs[i].shape.end(), [](int64_t dimension) { return dimension <= 0; })) {
      return absl::InvalidArgumentError("Float input dimensions must be positive");
    }
    auto expected_count = checked_element_count(inputs[i].shape);
    if (!expected_count.ok())
      return expected_count.status();
    if (inputs[i].data == nullptr || inputs[i].element_count != *expected_count) {
      return absl::InvalidArgumentError("Float input data length does not match its tensor shape");
    }
  }

  try {
    auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<Ort::Value> input_values;
    std::vector<const char*> input_names;
    input_values.reserve(inputs.size());
    input_names.reserve(inputs.size());
    for (const auto& input : inputs) {
      input_values.push_back(
          Ort::Value::CreateTensor<float>(
              memory, const_cast<float*>(input.data), input.element_count, input.shape.data(), input.shape.size()));
      input_names.push_back(input.name.c_str());
    }
    std::vector<const char*> output_names;
    output_names.reserve(outputs_.size());
    for (const auto& output : outputs_)
      output_names.push_back(output.name.c_str());
    Ort::RunOptions run_options;
    RunCancellationMonitor cancellation_monitor(&run_options, is_cancelled);
    auto values = session_->Run(
        run_options,
        input_names.data(),
        input_values.data(),
        input_values.size(),
        output_names.data(),
        output_names.size());
    if (values.size() != outputs_.size()) {
      return absl::InternalError("ONNX Runtime returned an unexpected output count");
    }
    std::vector<Tensor> tensors;
    tensors.reserve(values.size());
    for (size_t i = 0; i < values.size(); ++i) {
      auto info = values[i].GetTensorTypeAndShapeInfo();
      if (info.GetElementType() != outputs_[i].type) {
        return absl::InternalError("ONNX Runtime returned an unexpected output type");
      }
      auto output_status = validate_shape(info.GetShape(), outputs_[i].dimensions);
      if (!output_status.ok())
        return output_status;
      tensors.emplace_back(std::move(values[i]));
    }
    return tensors;
  } catch (const Ort::Exception& error) {
    if (is_cancelled && is_cancelled()) {
      return absl::CancelledError("ONNX inference cancelled");
    }
    return ort_error("ONNX inference failed", error);
  }
}

} // namespace hm::onnx
