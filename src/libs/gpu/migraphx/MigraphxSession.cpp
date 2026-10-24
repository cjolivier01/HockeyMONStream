#include "hstream/src/libs/gpu/migraphx/MigraphxSession.h"

#include <migraphx/migraphx.hpp>

#include <cstdio>
#include <map>
#include <optional>
#include <stdexcept>
#include <utility>

namespace hm::gpu {
namespace {
class Failure : public std::runtime_error {
 public:
  explicit Failure(absl::Status status) : std::runtime_error(status.ToString()), status(std::move(status)) {}
  absl::Status status;
};
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw Failure(absl::InvalidArgumentError(message));
}
void Hip(hipError_t error, const char* operation) {
  if (error != hipSuccess)
    throw Failure(absl::InternalError(std::string(operation) + ": " + hipGetErrorString(error)));
}
void Cleanup(hipError_t error, const char* operation) noexcept {
  if (error != hipSuccess)
    std::fprintf(stderr, "MIGraphX cleanup %s: %s\n", operation, hipGetErrorString(error));
}
class DeviceScope {
 public:
  explicit DeviceScope(int device) {
    Hip(hipGetDevice(&previous_), "hipGetDevice");
    Hip(hipSetDevice(device), "hipSetDevice");
  }
  ~DeviceScope() {
    Cleanup(hipSetDevice(previous_), "restore device");
  }

 private:
  int previous_;
};
size_t Elements(const std::vector<size_t>& dimensions, size_t maximum_bytes) {
  Require(!dimensions.empty(), "Scalar or unspecified model inputs are unsupported");
  size_t elements = 1;
  for (size_t dimension : dimensions) {
    Require(dimension && dimension <= maximum_bytes / sizeof(float) / elements, "Model tensor exceeds I/O bound");
    elements *= dimension;
  }
  return elements;
}
MigraphxSession::Tensor Describe(const migraphx::shape& shape, void* data, size_t maximum_bytes) {
  Require(shape.type() == migraphx_shape_float_type && shape.standard(), "Model bindings must be contiguous FP32");
  auto dimensions = shape.lengths();
  const auto elements = Elements(dimensions, maximum_bytes);
  Require(shape.bytes() == elements * sizeof(float), "Unexpected model tensor layout");
  return {static_cast<float*>(data), std::move(dimensions), elements};
}
} // namespace

struct MigraphxSession::Impl {
  int device;
  size_t maximum_io_bytes;
  hipStream_t stream{nullptr};
  bool retired{false};
  std::optional<migraphx::program> program;
  std::optional<migraphx::program_parameters> parameters;
  std::optional<migraphx::arguments> results;
  std::map<std::string, Tensor> bindings;
  std::vector<Tensor> inputs, outputs;

  Impl(int device, size_t maximum_io_bytes) : device(device), maximum_io_bytes(maximum_io_bytes) {}
  ~Impl() {
    try {
      DeviceScope scope(device);
      if (stream)
        Cleanup(hipStreamSynchronize(stream), "drain stream");
      results.reset();
      parameters.reset();
      program.reset();
      for (const auto& binding : bindings)
        Cleanup(hipFree(binding.second.data), "free binding");
    } catch (const std::exception& error) {
      std::fprintf(stderr, "MIGraphX cleanup: %s\n", error.what());
    }
  }
};

MigraphxSession::MigraphxSession(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
MigraphxSession::~MigraphxSession() = default;

absl::StatusOr<std::unique_ptr<MigraphxSession>> MigraphxSession::Compile(
    const std::filesystem::path& onnx,
    const std::vector<ModelInputShape>& inputs,
    Precision precision,
    int device,
    size_t maximum_io_bytes) {
  try {
    Require(!inputs.empty() && maximum_io_bytes >= sizeof(float), "Model inputs and I/O byte bound are required");
    Require(precision == Precision::kFloat32 || precision == Precision::kFloat16, "Unsupported model precision");
    migraphx::onnx_options options;
    std::map<std::string, std::vector<size_t>> requested;
    for (const auto& input : inputs) {
      Require(
          !input.name.empty() && requested.emplace(input.name, input.dimensions).second,
          "Invalid/duplicate input name");
      (void)Elements(input.dimensions, maximum_io_bytes);
      options.set_input_parameter_shape(input.name, input.dimensions);
    }
    DeviceScope scope(device);
    auto impl = std::make_unique<Impl>(device, maximum_io_bytes);
    impl->program.emplace(migraphx::parse_onnx(onnx.c_str(), options));
    const auto parsed = impl->program->get_parameter_shapes();
    Require(parsed.size() == inputs.size(), "Every ONNX input must have an explicit shape");
    for (const auto* name : parsed.names()) {
      const auto expected = requested.find(name);
      Require(expected != requested.end(), std::string("Unexpected ONNX input: ") + name);
      Require(
          Describe(parsed[name], nullptr, maximum_io_bytes).dimensions == expected->second,
          "ONNX input shape mismatch");
    }
    if (precision == Precision::kFloat16)
      migraphx::quantize_fp16(*impl->program);
    migraphx::compile_options compile;
    compile.set_offload_copy(false);
    impl->program->compile(migraphx::target("gpu"), compile);
    impl->parameters.emplace();
    const auto compiled = impl->program->get_parameter_shapes();
    size_t remaining = maximum_io_bytes;
    for (const auto* name : compiled.names()) {
      const auto shape = compiled[name];
      auto tensor = Describe(shape, nullptr, remaining);
      const auto bytes = tensor.elements * sizeof(float);
      // Insert first so every successful allocation belongs to failure cleanup.
      auto& binding = impl->bindings.emplace(name, tensor).first->second;
      Hip(hipMalloc(reinterpret_cast<void**>(&binding.data), bytes), "allocate model binding");
      remaining -= bytes;
      impl->parameters->add(name, migraphx::argument(shape, binding.data));
    }
    for (const auto& input : inputs) {
      const auto binding = impl->bindings.find(input.name);
      Require(
          binding != impl->bindings.end() && binding->second.dimensions == input.dimensions,
          "Compiler changed the input contract");
      impl->inputs.push_back(binding->second);
    }
    for (const auto& shape : impl->program->get_output_shapes()) {
      impl->outputs.push_back(Describe(shape, nullptr, maximum_io_bytes));
    }
    return std::unique_ptr<MigraphxSession>(new MigraphxSession(std::move(impl)));
  } catch (const Failure& error) {
    return error.status;
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("MIGraphX preparation: ") + error.what());
  }
}

size_t MigraphxSession::input_count() const {
  return impl_->inputs.size();
}
const MigraphxSession::Tensor& MigraphxSession::input(size_t index) const {
  return impl_->inputs.at(index);
}
size_t MigraphxSession::output_count() const {
  return impl_->outputs.size();
}
const MigraphxSession::Tensor& MigraphxSession::output(size_t index) const {
  return impl_->outputs.at(index);
}

absl::Status MigraphxSession::Enqueue(hipStream_t stream) {
  if (!stream || stream == hipStreamLegacy || stream == hipStreamPerThread ||
      (impl_->stream && impl_->stream != stream)) {
    return absl::InvalidArgumentError("MIGraphX requires one explicit caller stream per session");
  }
  if (impl_->retired)
    return absl::FailedPreconditionError("MIGraphX session retired after an enqueue failure");
  try {
    DeviceScope scope(impl_->device);
    hipDevice_t stream_device;
    Hip(hipStreamGetDevice(stream, &stream_device), "get stream device");
    Require(stream_device == impl_->device, "Inference stream belongs to another device");
    impl_->stream = stream;
    impl_->results.emplace(impl_->program->run_async(*impl_->parameters, stream));
    Require(impl_->results->size() == impl_->outputs.size(), "Model changed its output count");
    for (size_t i = 0; i < impl_->outputs.size(); ++i) {
      const auto argument = (*impl_->results)[i];
      const auto output = Describe(argument.get_shape(), argument.data(), impl_->maximum_io_bytes);
      Require(output.data && output.dimensions == impl_->outputs[i].dimensions, "Model changed its output contract");
      impl_->outputs[i] = output;
    }
    return absl::OkStatus();
  } catch (const std::exception& error) {
    impl_->retired = true;
    return absl::InternalError(std::string("MIGraphX enqueue: ") + error.what());
  }
}

} // namespace hm::gpu
