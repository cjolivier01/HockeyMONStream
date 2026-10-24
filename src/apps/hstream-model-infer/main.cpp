#include "Protocol.h"

#include <cuda_runtime_api.h>
#include <hstream_player_tensorrt/NvInfer.h>
#include <poll.h>
#include <sys/syscall.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {
namespace wire = hm::model_infer;
void Require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t code) {
  if (code != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(code));
}
class ParentProcess {
 public:
  explicit ParentProcess(pid_t parent) {
    // PDEATHSIG follows the creating thread, which may be a short-lived Aspen
    // worker. A pidfd follows the owning Python process until its last thread
    // exits and stays bound to that identity even if its PID is later reused.
    descriptor_ = static_cast<int>(::syscall(SYS_pidfd_open, parent, 0));
    Require(descriptor_ >= 0, "Cannot monitor model inference parent process: " + std::string(std::strerror(errno)));
    try {
      Require(::getppid() == parent, "Model inference parent exited");
      watcher_ = std::thread([this] {
        while (!stopping_.load()) {
          pollfd watched{descriptor_, POLLIN, 0};
          const int result = ::poll(&watched, 1, 50);
          if (result == 0 || (result < 0 && errno == EINTR))
            continue;
          if (result < 0 || !(watched.revents & POLLIN)) {
            constexpr char message[] = "hstream-model-infer: Parent process monitoring failed\n";
            const auto reported = ::write(STDERR_FILENO, message, sizeof(message) - 1);
            (void)reported;
          }
          // Terminate even when the main thread is blocked on input or CUDA.
          std::raise(SIGTERM);
          std::_Exit(EXIT_FAILURE);
        }
      });
    } catch (...) {
      ::close(descriptor_);
      throw;
    }
  }
  ~ParentProcess() {
    stopping_.store(true);
    watcher_.join();
    ::close(descriptor_);
  }

 private:
  int descriptor_;
  std::atomic<bool> stopping_{false};
  std::thread watcher_;
};
class Logger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* message) noexcept override {
    if (severity <= Severity::kWARNING)
      std::cerr << message << '\n';
  }
};
class Buffer {
 public:
  explicit Buffer(size_t bytes) {
    Cuda(cudaMalloc(&value_, bytes));
  }
  ~Buffer() {
    if (value_)
      cudaFree(value_);
  }
  void* data() const {
    return value_;
  }

 private:
  void* value_{};
};
class Stream {
 public:
  Stream() {
    Cuda(cudaStreamCreate(&value_));
  }
  ~Stream() {
    cudaStreamSynchronize(value_);
    cudaStreamDestroy(value_);
  }
  operator cudaStream_t() const {
    return value_;
  }

 private:
  cudaStream_t value_{};
};
int Integer(const std::string& value, int minimum) {
  size_t length;
  const auto result = std::stol(value, &length);
  Require(
      length == value.size() && result >= minimum && result <= std::numeric_limits<int>::max(),
      "Invalid model inference integer argument");
  return static_cast<int>(result);
}
struct Arguments {
  std::string engine;
  int gpu{};
  int parent{};
  bool describe{};
};
Arguments Parse(int argc, char** argv) {
  Arguments args;
  for (int index = 1; index < argc; ++index) {
    const std::string key = argv[index];
    if (key == "--describe") {
      args.describe = true;
      continue;
    }
    Require(index + 1 < argc, "Missing model inference argument value");
    const std::string value = argv[++index];
    if (key == "--engine")
      args.engine = value;
    else if (key == "--gpu-id")
      args.gpu = Integer(value, 0);
    else if (key == "--parent-pid")
      args.parent = Integer(value, 1);
    else
      throw std::runtime_error("Unknown model inference argument: " + key);
  }
  Require(!args.engine.empty(), "--engine is required");
  return args;
}
class Appearance {
 public:
  explicit Appearance(const Arguments& args) {
    const std::filesystem::path path(args.engine);
    Require(std::filesystem::is_regular_file(path), "Prepared TensorRT engine is not a regular file");
    const auto size = std::filesystem::file_size(path);
    Require(size > 0 && size <= (2ULL << 30), "Prepared TensorRT engine size is outside its bound");
    std::vector<char> plan(size);
    std::ifstream input(path, std::ios::binary);
    input.read(plan.data(), plan.size());
    Require(input.good() && input.peek() == std::char_traits<char>::eof(), "Cannot read complete prepared engine");
    Cuda(cudaSetDevice(args.gpu));
    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    Require(bool(runtime_), "Cannot create native TensorRT runtime");
    engine_.reset(runtime_->deserializeCudaEngine(plan.data(), plan.size()));
    Require(bool(engine_), "Cannot deserialize the exact prepared engine with the native runtime");
    Require(
        engine_->getNbIOTensors() == 2 && engine_->getNbOptimizationProfiles() == 1,
        "Appearance engine must have one input, one output and one profile");
    for (int index = 0; index < engine_->getNbIOTensors(); ++index) {
      const char* name = engine_->getIOTensorName(index);
      Require(
          engine_->getTensorDataType(name) == nvinfer1::DataType::kFLOAT &&
              engine_->getTensorLocation(name) == nvinfer1::TensorLocation::kDEVICE &&
              engine_->getTensorFormat(name) == nvinfer1::TensorFormat::kLINEAR,
          "Appearance bindings must be linear float32 device tensors");
      auto& selected = engine_->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT ? input_name_ : output_name_;
      Require(selected.empty(), "Appearance engine has duplicate input/output roles");
      selected = name;
    }
    Require(!input_name_.empty() && !output_name_.empty(), "Appearance engine is missing an input/output binding");
    shape_ = engine_->getTensorShape(input_name_.c_str());
    Require(
        shape_.nbDims == 4 && shape_.d[1] == 3 && shape_.d[2] == 256 && shape_.d[3] == 128,
        "Appearance engine input must be [N,3,256,128]");
    dynamic_ = shape_.d[0] == -1;
    minimum_ = maximum_ = static_cast<int>(shape_.d[0]);
    if (dynamic_) {
      for (const auto selector : {nvinfer1::OptProfileSelector::kMIN, nvinfer1::OptProfileSelector::kMAX}) {
        const auto profile = engine_->getProfileShape(input_name_.c_str(), 0, selector);
        Require(
            profile.nbDims == 4 && profile.d[1] == 3 && profile.d[2] == 256 && profile.d[3] == 128,
            "Appearance profile changes the crop dimensions");
        (selector == nvinfer1::OptProfileSelector::kMIN ? minimum_ : maximum_) = profile.d[0];
      }
    }
    Require(
        minimum_ >= 1 && maximum_ >= minimum_ && maximum_ <= int(wire::kMaximumBatch),
        "Appearance engine batch profile exceeds its bound");
    context_.reset(engine_->createExecutionContext());
    Require(bool(context_), "Cannot create native appearance execution context");
    input_buffer_ = std::make_unique<Buffer>(maximum_ * wire::kInputFloats * sizeof(float));
    output_buffer_ = std::make_unique<Buffer>(maximum_ * wire::kOutputFloats * sizeof(float));
    stream_ = std::make_unique<Stream>();
    Require(
        context_->setTensorAddress(input_name_.c_str(), input_buffer_->data()) &&
            context_->setTensorAddress(output_name_.c_str(), output_buffer_->data()),
        "Cannot bind native appearance tensors");
    SetBatch(maximum_);
  }
  int minimum() const {
    return minimum_;
  }
  int maximum() const {
    return maximum_;
  }
  std::vector<float> Infer(const std::vector<float>& values, int count) {
    wire::Finite(values);
    std::vector<float> result(count * wire::kOutputFloats);
    for (int first = 0; first < count; first += maximum_) {
      const int selected = std::min(maximum_, count - first);
      const int batch = std::max(minimum_, selected);
      SetBatch(batch);
      // Only bounded, preprocessed appearance crops cross the process ABI.
      // The frame stays in Python's GPU pipeline; host IPC is necessary because
      // its ambient TensorRT runtime cannot deserialize the native SDK engine.
      Cuda(cudaMemsetAsync(input_buffer_->data(), 0, batch * wire::kInputFloats * sizeof(float), *stream_));
      Cuda(cudaMemcpyAsync(
          input_buffer_->data(),
          values.data() + first * wire::kInputFloats,
          selected * wire::kInputFloats * sizeof(float),
          cudaMemcpyHostToDevice,
          *stream_));
      Require(context_->enqueueV3(*stream_), "Native appearance inference failed");
      Cuda(cudaMemcpyAsync(
          result.data() + first * wire::kOutputFloats,
          output_buffer_->data(),
          selected * wire::kOutputFloats * sizeof(float),
          cudaMemcpyDeviceToHost,
          *stream_));
      Cuda(cudaStreamSynchronize(*stream_));
    }
    wire::Finite(result);
    return result;
  }

 private:
  void SetBatch(int count) {
    if (dynamic_) {
      auto shape = shape_;
      shape.d[0] = count;
      Require(context_->setInputShape(input_name_.c_str(), shape), "Batch lies outside the native engine profile");
    }
    const auto output = context_->getTensorShape(output_name_.c_str());
    Require(
        output.nbDims >= 2 && output.nbDims <= 8 && output.d[0] == count,
        "Appearance engine output must begin with the execution batch");
    size_t features = 1;
    for (int index = 1; index < output.nbDims; ++index) {
      Require(
          output.d[index] > 0 && output.d[index] <= int64_t(wire::kOutputFloats),
          "Appearance output feature dimension is invalid");
      features *= output.d[index];
      Require(features <= wire::kOutputFloats, "Appearance engine output exceeds 256 features");
    }
    Require(features == wire::kOutputFloats, "Appearance engine must output exactly 256 features per crop");
  }
  Logger logger_;
  std::unique_ptr<nvinfer1::IRuntime> runtime_;
  std::unique_ptr<nvinfer1::ICudaEngine> engine_;
  std::unique_ptr<nvinfer1::IExecutionContext> context_;
  std::string input_name_, output_name_;
  nvinfer1::Dims shape_{};
  bool dynamic_{};
  int minimum_{}, maximum_{};
  std::unique_ptr<Buffer> input_buffer_, output_buffer_;
  std::unique_ptr<Stream> stream_;
};
} // namespace
int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "hstream-model-infer --engine PREPARED.engine [--gpu-id N] [--parent-pid PID] [--describe]\n"
                   "Binary requests: little-endian uint32 batch (1..32), then float32 [N,3,256,128].\n"
                   "Responses: little-endian uint32 batch, then float32 [N,256]. Zero/EOF ends the session.\n";
      return 0;
    }
    const auto args = Parse(argc, argv);
    const auto parent = ::getppid();
    const char* inherited = std::getenv("HSTREAM_UI_PARENT_PID");
    const int expected = args.parent ? args.parent : inherited ? Integer(inherited, 1) : parent;
    Require(expected == parent, "Stale model inference parent PID");
    std::signal(SIGPIPE, SIG_IGN);
    std::signal(SIGTERM, SIG_DFL);
    ParentProcess owner(parent);
    Appearance model(args);
    if (args.describe) {
      std::cout << "{\"version\":1,\"max_request_batch\":32,\"engine_batch_min\":" << model.minimum()
                << ",\"engine_batch_max\":" << model.maximum()
                << ",\"input_shape\":[-1,3,256,128],\"output_shape\":[-1,256],\"dtype\":\"float32\"}\n";
      return 0;
    }
    while (const auto count = wire::ReadBatch(STDIN_FILENO)) {
      std::vector<float> input(count * wire::kInputFloats);
      wire::ReadExact(STDIN_FILENO, input.data(), input.size() * sizeof(float));
      const auto output = model.Infer(input, count);
      wire::WriteBatch(STDOUT_FILENO, count);
      wire::WriteExact(STDOUT_FILENO, output.data(), output.size() * sizeof(float));
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "hstream-model-infer: " << error.what() << '\n';
    return 1;
  }
}
