#include "Bf16Engine.h"

#include "EnginePrecision.h"

#include <cuda_runtime_api.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <hstream_player_tensorrt/NvInfer.h>
#include <hstream_player_tensorrt/NvOnnxParser.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace hm::inference {
namespace {
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
bool cancelled(const Bf16EngineOptions& options) {
  return options.cancelled && options.cancelled();
}
void check_cancelled(const Bf16EngineOptions& options) {
  require(!cancelled(options), "BF16 engine preparation cancelled");
}
class Logger : public nvinfer1::ILogger {
 public:
  explicit Logger(const Bf16EngineOptions& options) : options_(options) {}
  void log(Severity severity, const char* message) noexcept override {
    if (severity > Severity::kINFO)
      return;
    const std::string text = std::string("TensorRT BF16: ") + message;
    if (options_.progress)
      options_.progress(text);
    else
      std::cerr << text << '\n';
  }

 private:
  const Bf16EngineOptions& options_;
};

class Device {
 public:
  explicit Device(unsigned gpu) {
    require(cudaGetDevice(&previous_) == cudaSuccess, "Cannot inspect the current CUDA device");
    require(cudaSetDevice(gpu) == cudaSuccess, "Cannot select the detector CUDA device");
  }
  ~Device() {
    cudaSetDevice(previous_);
  }

 private:
  int previous_;
};

void check_runtime(const Bf16EngineOptions& options) {
  check_cancelled(options);
  require(
      TensorRtVersionMatches(getInferLibVersion(), NV_TENSORRT_MAJOR, NV_TENSORRT_MINOR),
      "BF16 builder SDK does not match the loaded TensorRT runtime");
  // Loading DeepStream also registers TensorRT plugin creators. They remain
  // in the process registry, so closing this handle would leave dangling
  // creators and crash when the pipeline later loads the nvinfer plugin.
  static void* library = dlopen("libnvds_infer.so", RTLD_NOW | RTLD_LOCAL);
  auto version = library ? reinterpret_cast<int (*)()>(dlsym(library, "getInferLibVersion")) : nullptr;
  const bool matches = version && version() == getInferLibVersion();
  require(matches, "BF16 builder runtime does not match DeepStream's inference runtime");
  require(options.batch > 0, "BF16 engine batch must be positive");
  cudaDeviceProp device{};
  require(cudaGetDeviceProperties(&device, options.gpu) == cudaSuccess, "Cannot inspect the detector GPU");
  require(device.major >= 8, "BF16 detection requires an Ampere or newer NVIDIA GPU");
}

#if NV_TENSORRT_MAJOR >= 9
class Progress : public nvinfer1::IProgressMonitor {
 public:
  explicit Progress(const Bf16EngineOptions& options) : options_(options) {}
  void phaseStart(const char*, const char*, int32_t) noexcept override {}
  void phaseFinish(const char*) noexcept override {}
  bool stepComplete(const char*, int32_t) noexcept override {
    return !cancelled(options_);
  }

 private:
  const Bf16EngineOptions& options_;
};
#endif

std::string inspect(nvinfer1::ICudaEngine& engine, const Bf16EngineOptions& options) {
  int inputs = 0;
  for (int i = 0; i < engine.getNbIOTensors(); ++i) {
    const char* name = engine.getIOTensorName(i);
    require(
        engine.getTensorDataType(name) == nvinfer1::DataType::kFLOAT,
        "BF16 detector input/output must remain FP32 for the inference parser");
    if (engine.getTensorIOMode(name) != nvinfer1::TensorIOMode::kINPUT)
      continue;
    ++inputs;
    const auto dims = engine.getTensorShape(name);
    require(
        dims.nbDims == 4 && dims.d[1] == 3 && dims.d[2] > 0 && dims.d[3] > 0,
        "BF16 detector must have a fixed-spatial NCHW input");
    if (dims.d[0] < 0) {
      const auto minimum = engine.getProfileShape(name, 0, nvinfer1::OptProfileSelector::kMIN);
      const auto maximum = engine.getProfileShape(name, 0, nvinfer1::OptProfileSelector::kMAX);
      require(
          minimum.nbDims == 4 && maximum.nbDims == 4 && minimum.d[0] == 1 && maximum.d[0] >= options.batch,
          "Cached BF16 engine cannot serve the selected batch size");
    } else {
      require(dims.d[0] == options.batch, "Cached BF16 engine has a different static batch size");
    }
  }
  require(inputs == 1, "BF16 detector must have exactly one input");
  std::unique_ptr<nvinfer1::IEngineInspector> inspector(engine.createEngineInspector());
  const char* details = inspector ? inspector->getEngineInformation(nvinfer1::LayerInformationFormat::kJSON) : nullptr;
  require(
      details && EngineUsesPrecision(YAML::Load(details), "bf16"),
      "Engine inspection found no BF16 tensors; refusing a different precision");
  return details;
}

void write_file(const fs::path& file, const void* data, size_t size) {
  const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  require(fd >= 0, "Cannot create BF16 engine staging file: " + file.string());
  size_t offset = 0;
  while (offset < size) {
    const auto n = ::write(fd, static_cast<const char*>(data) + offset, size - offset);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0) {
      ::close(fd);
      throw std::runtime_error("Cannot write BF16 engine staging file");
    }
    offset += n;
  }
  const bool synced = ::fsync(fd) == 0;
  const bool closed = ::close(fd) == 0;
  require(synced && closed, "Cannot sync BF16 engine staging file");
}
absl::Status failure(const std::exception& error, const Bf16EngineOptions& options) {
  return cancelled(options) ? absl::CancelledError("BF16 engine preparation cancelled")
                            : absl::FailedPreconditionError(error.what());
}
} // namespace

absl::Status ValidateBf16Engine(const fs::path& path, const Bf16EngineOptions& options) {
  try {
    check_runtime(options);
    Device device(options.gpu);
    require(fs::is_regular_file(fs::symlink_status(path)), "BF16 engine is missing: " + path.string());
    const auto size = fs::file_size(path);
    require(size > 0 && size <= (2ULL << 30), "BF16 engine is empty or oversized");
    std::vector<char> bytes(size);
    std::ifstream input(path, std::ios::binary);
    require(bool(input.read(bytes.data(), bytes.size())), "Cannot read the cached BF16 engine");
    Logger logger(options);
    std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(logger));
    std::unique_ptr<nvinfer1::ICudaEngine> engine(
        runtime ? runtime->deserializeCudaEngine(bytes.data(), bytes.size()) : nullptr);
    require(bool(engine), "Cached BF16 engine is incompatible with this GPU/runtime or damaged");
    inspect(*engine, options);
    check_cancelled(options);
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return failure(error, options);
  }
}

absl::Status BuildBf16Engine(const fs::path& onnx, const fs::path& path, const Bf16EngineOptions& options) {
  fs::path staging;
  try {
    check_runtime(options);
    Device device(options.gpu);
#if NV_TENSORRT_MAJOR < 9 || NV_TENSORRT_MAJOR >= 11
    throw std::runtime_error(
        "Automatic BF16 conversion requires TensorRT 9 or 10; this runtime requires a typed BF16 ONNX");
#else
    Logger logger(options);
    Progress progress(options);
    std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
    require(bool(builder), "Cannot create the BF16 engine builder");
    const auto flags = NV_TENSORRT_MAJOR < 10
        ? 1U << static_cast<uint32_t>(nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH)
        : 0U;
    std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(flags));
    require(bool(network), "Cannot create the BF16 network");
    std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger));
    require(
        parser && parser->parseFromFile(onnx.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)),
        "Cannot parse the selected BF16 detector ONNX: " + onnx.string());
    require(network->getNbInputs() == 1, "BF16 detector must have one input");
    auto* input = network->getInput(0);
    auto dims = input->getDimensions();
    require(
        dims.nbDims == 4 && dims.d[1] == 3 && dims.d[2] > 0 && dims.d[3] > 0,
        "BF16 detector must have a fixed-spatial NCHW input");
    require(input->getType() == nvinfer1::DataType::kFLOAT, "BF16 detector input must remain FP32");
    for (int i = 0; i < network->getNbOutputs(); ++i)
      require(network->getOutput(i)->getType() == nvinfer1::DataType::kFLOAT, "BF16 detector output must remain FP32");
    std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
    require(bool(config), "Cannot create the BF16 builder configuration");
    config->setFlag(nvinfer1::BuilderFlag::kBF16);
    config->setProfilingVerbosity(nvinfer1::ProfilingVerbosity::kDETAILED);
    config->setProgressMonitor(&progress);
    if (options.workspace_mib)
      config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, uint64_t(*options.workspace_mib) << 20);
    if (dims.d[0] == -1) {
      auto* profile = builder->createOptimizationProfile();
      dims.d[0] = 1;
      require(
          profile && profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMIN, dims),
          "Invalid BF16 minimum batch");
      dims.d[0] = options.batch;
      require(
          profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kOPT, dims) &&
              profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMAX, dims) &&
              config->addOptimizationProfile(profile) >= 0,
          "Invalid BF16 maximum batch");
    } else {
      require(dims.d[0] == options.batch, "Selected batch differs from the detector's fixed ONNX batch");
    }
    check_cancelled(options);
    std::unique_ptr<nvinfer1::IHostMemory> bytes(builder->buildSerializedNetwork(*network, *config));
    check_cancelled(options);
    require(bool(bytes), "TensorRT failed to build the selected BF16 detector");
    std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(logger));
    std::unique_ptr<nvinfer1::ICudaEngine> engine(
        runtime ? runtime->deserializeCudaEngine(bytes->data(), bytes->size()) : nullptr);
    require(bool(engine), "Cannot deserialize the generated BF16 engine");
    const auto details = inspect(*engine, options);
    auto pattern = (path.parent_path() / ".bf16-preparing-XXXXXX").string();
    require(::mkdtemp(pattern.data()) != nullptr, "Cannot stage the BF16 engine");
    staging = pattern;
    write_file(staging / "engine", bytes->data(), bytes->size());
    write_file(staging / "layers.json", details.data(), details.size());
    check_cancelled(options);
    fs::rename(staging / "layers.json", path.string() + ".layers.json");
    fs::rename(staging / "engine", path);
    fs::remove(staging);
    return absl::OkStatus();
#endif
  } catch (const std::exception& error) {
    if (!staging.empty()) {
      std::error_code ignored;
      fs::remove_all(staging, ignored);
    }
    return failure(error, options);
  }
}
} // namespace hm::inference
