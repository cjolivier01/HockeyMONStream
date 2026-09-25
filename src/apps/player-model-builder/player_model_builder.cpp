#include "DeepStreamTrackerBuilder.h"

#include <cuda_runtime_api.h>
#include <hstream_player_tensorrt/NvInfer.h>
#include <hstream_player_tensorrt/NvOnnxParser.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr size_t kMaximumTensorBytes = 512ULL << 20;
constexpr size_t kMaximumEngineBytes = 2ULL << 30;
class Logger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* message) noexcept override {
    if (severity <= Severity::kWARNING)
      std::cerr << message << '\n';
  }
};
void Cuda(cudaError_t result) {
  if (result != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(result));
}
std::string Json(const std::string& value) {
  std::ostringstream output;
  output << '"';
  for (const unsigned char c : value) {
    if (c == '"' || c == '\\')
      output << '\\' << c;
    else if (c < 32)
      throw std::runtime_error("Unexpected control character in JSON value");
    else
      output << c;
  }
  output << '"';
  return output.str();
}
struct Arguments {
  bool runtime_info = false;
  std::string onnx, engine, input, outputs, contract, tracker_config, tracker_library, precision = "fp16";
  int max_batch = 8, batch = 1, workspace_mb = 256, gpu_id = 0, parent_pid = 0;
};
int Positive(const std::string& value, int maximum) {
  size_t used = 0;
  const int result = std::stoi(value, &used);
  if (used != value.size() || result < 1 || result > maximum)
    throw std::runtime_error("Invalid positive integer");
  return result;
}
Arguments Parse(int argc, char** argv) {
  Arguments args;
  for (int i = 1; i < argc; ++i) {
    const std::string key = argv[i];
    if (key == "--runtime-info") {
      args.runtime_info = true;
      continue;
    }
    if (key == "--help") {
      std::cout
          << "player-model-builder --runtime-info [--gpu-id N]\n"
             "player-model-builder --onnx MODEL --engine FILE [--precision fp16|fp32] [--max-batch N] [--contract FILE] [--gpu-id N]\n"
             "player-model-builder --engine FILE --input FLOAT32_FILE --batch N --outputs DIRECTORY\n";
      std::exit(0);
    }
    if (++i >= argc)
      throw std::runtime_error("Missing value for " + key);
    const std::string value = argv[i];
    if (key == "--parent-pid")
      args.parent_pid = Positive(value, std::numeric_limits<int>::max());
    else if (key == "--tracker-config")
      args.tracker_config = value;
    else if (key == "--tracker-library")
      args.tracker_library = value;
    else if (key == "--onnx")
      args.onnx = value;
    else if (key == "--contract")
      args.contract = value;
    else if (key == "--gpu-id") {
      size_t used = 0;
      args.gpu_id = std::stoi(value, &used);
      if (used != value.size() || args.gpu_id < 0)
        throw std::runtime_error("GPU id must be nonnegative");
    } else if (key == "--engine")
      args.engine = value;
    else if (key == "--input")
      args.input = value;
    else if (key == "--outputs")
      args.outputs = value;
    else if (key == "--precision")
      args.precision = value;
    else if (key == "--max-batch")
      args.max_batch = Positive(value, 64);
    else if (key == "--batch")
      args.batch = Positive(value, 64);
    else if (key == "--workspace-mb")
      args.workspace_mb = Positive(value, 4096);
    else
      throw std::runtime_error("Unknown argument " + key);
  }
  if (args.precision != "fp16" && args.precision != "fp32")
    throw std::runtime_error("Precision must be fp16 or fp32");
  if (!args.tracker_config.empty() && args.tracker_library.empty())
    throw std::runtime_error("--tracker-config requires --tracker-library");
  if (!args.runtime_info && args.tracker_config.empty() && args.engine.empty())
    throw std::runtime_error("--engine is required");
  if (args.onnx.empty() && !args.runtime_info && args.tracker_config.empty() && args.contract.empty() &&
      (args.input.empty() || args.outputs.empty()))
    throw std::runtime_error("Engine validation requires --input and --outputs");
  return args;
}
std::string Version(int value) {
  return std::to_string(value / 10000) + "." + std::to_string(value / 100 % 100) + "." + std::to_string(value % 100);
}
std::string RuntimeInfo(int gpu_id) {
  const int runtime_version = getInferLibVersion();
  const int compiled_version = NV_TENSORRT_MAJOR * 10000 + NV_TENSORRT_MINOR * 100 + NV_TENSORRT_PATCH;
  if (runtime_version != compiled_version || getInferLibBuildVersion() != NV_TENSORRT_BUILD)
    throw std::runtime_error("TensorRT header and runtime versions differ");
  cudaDeviceProp device{};
  Cuda(cudaSetDevice(gpu_id));
  Cuda(cudaGetDeviceProperties(&device, gpu_id));
  int cuda_version = 0;
  Cuda(cudaRuntimeGetVersion(&cuda_version));
  std::ostringstream result;
  result << "{\"tensorrt_version\":" << Json(Version(runtime_version)) << ",\"cuda_runtime_version\":" << cuda_version
         << ",\"gpu_name\":" << Json(device.name)
         << ",\"compute_capability\":" << Json(std::to_string(device.major) + "." + std::to_string(device.minor))
         << ",\"tensorrt_build_version\":" << getInferLibBuildVersion() << "}";
  return result.str();
}
size_t TensorBytes(nvinfer1::Dims dims) {
  size_t bytes = sizeof(float);
  if (dims.nbDims < 1 || dims.nbDims > 8)
    throw std::runtime_error("Invalid tensor rank");
  for (int i = 0; i < dims.nbDims; ++i) {
    if (dims.d[i] <= 0 || static_cast<uint64_t>(dims.d[i]) > kMaximumTensorBytes / bytes)
      throw std::runtime_error("Unresolved or oversized tensor dimensions");
    bytes *= dims.d[i];
  }
  return bytes;
}
std::vector<char> Read(const std::filesystem::path& file, size_t maximum, size_t exact = 0) {
  if (!std::filesystem::is_regular_file(file))
    throw std::runtime_error("Not a regular file: " + file.string());
  const auto size = std::filesystem::file_size(file);
  if (!size || size > maximum || (exact && size != exact))
    throw std::runtime_error("Invalid file size: " + file.string());
  std::vector<char> data(size);
  std::ifstream stream(file, std::ios::binary);
  if (!stream.read(data.data(), data.size()))
    throw std::runtime_error("Cannot read " + file.string());
  return data;
}
void Write(const std::filesystem::path& path, const void* data, size_t bytes) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output.write(static_cast<const char*>(data), bytes))
    throw std::runtime_error("Cannot write " + path.string());
  output.close();
  if (!output)
    throw std::runtime_error("Cannot close " + path.string());
}
std::unique_ptr<nvinfer1::IHostMemory> Build(const Arguments& args, Logger& logger) {
  std::unique_ptr<nvinfer1::IBuilder> builder(nvinfer1::createInferBuilder(logger));
  if (!builder)
    throw std::runtime_error("Cannot create TensorRT builder");
  std::unique_ptr<nvinfer1::INetworkDefinition> network(builder->createNetworkV2(0));
  if (!network)
    throw std::runtime_error("Cannot create TensorRT network");
  std::unique_ptr<nvonnxparser::IParser> parser(nvonnxparser::createParser(*network, logger));
  if (!parser || !parser->parseFromFile(args.onnx.c_str(), static_cast<int>(nvinfer1::ILogger::Severity::kWARNING)))
    throw std::runtime_error("ONNX parsing failed");
  if (network->getNbInputs() != 1 || network->getNbOutputs() < 1 || network->getNbOutputs() > 8)
    throw std::runtime_error("Expected one input and 1..8 outputs");
  auto* input = network->getInput(0);
  if (input->getType() != nvinfer1::DataType::kFLOAT)
    throw std::runtime_error("Input binding must be float32");
  for (int i = 0; i < network->getNbOutputs(); ++i)
    if (network->getOutput(i)->getType() != nvinfer1::DataType::kFLOAT)
      throw std::runtime_error("Output bindings must be float32");
  auto dims = input->getDimensions();
  if (dims.nbDims < 2 || dims.d[0] != -1)
    throw std::runtime_error("Input must have only a dynamic batch axis");
  dims.d[0] = args.max_batch;
  TensorBytes(dims);
  std::unique_ptr<nvinfer1::IBuilderConfig> config(builder->createBuilderConfig());
  if (!config)
    throw std::runtime_error("Cannot create TensorRT builder configuration");
  config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, static_cast<size_t>(args.workspace_mb) << 20);
  if (args.precision == "fp16")
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
  else
    config->clearFlag(nvinfer1::BuilderFlag::kTF32);
  auto* profile = builder->createOptimizationProfile();
  if (!profile)
    throw std::runtime_error("Cannot create TensorRT optimization profile");
  dims.d[0] = 1;
  if (!profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMIN, dims))
    throw std::runtime_error("Invalid minimum batch shape");
  dims.d[0] = std::min(args.max_batch, 2);
  if (!profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kOPT, dims))
    throw std::runtime_error("Invalid optimum batch shape");
  dims.d[0] = args.max_batch;
  if (!profile->setDimensions(input->getName(), nvinfer1::OptProfileSelector::kMAX, dims) ||
      config->addOptimizationProfile(profile) < 0)
    throw std::runtime_error("Invalid maximum batch shape");
  std::unique_ptr<nvinfer1::IHostMemory> plan(builder->buildSerializedNetwork(*network, *config));
  if (!plan || plan->size() > kMaximumEngineBytes)
    throw std::runtime_error("Engine build failed or is oversized");
  return plan;
}
class CudaBuffer {
 public:
  explicit CudaBuffer(size_t bytes) {
    Cuda(cudaMalloc(&data_, bytes));
  }
  ~CudaBuffer() {
    if (data_)
      cudaFree(data_);
  }
  CudaBuffer(const CudaBuffer&) = delete;
  CudaBuffer& operator=(const CudaBuffer&) = delete;
  void* data() const {
    return data_;
  }

 private:
  void* data_ = nullptr;
};
class CudaStream {
 public:
  CudaStream() {
    Cuda(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking));
  }
  ~CudaStream() {
    cudaStreamSynchronize(stream_);
    cudaStreamDestroy(stream_);
  }
  operator cudaStream_t() const {
    return stream_;
  }

 private:
  cudaStream_t stream_{};
};
void ValidateContract(nvinfer1::ICudaEngine& engine, const Arguments& args) {
  if (args.contract.empty())
    return;
  const auto bytes = Read(args.contract, 1024 * 1024);
  const auto contract = YAML::Load(std::string(bytes.begin(), bytes.end()));
  const auto inputs = contract["inputs"];
  const auto outputs = contract["outputs"];
  if (!inputs.IsSequence() || inputs.size() != 1 || !outputs.IsSequence() || outputs.size() < 1 || outputs.size() > 8 ||
      engine.getNbIOTensors() != static_cast<int>(inputs.size() + outputs.size()) ||
      engine.getNbOptimizationProfiles() != 1)
    throw std::runtime_error("Engine IO/profile count does not match the selected model contract");
  std::set<std::string> names;
  for (int direction = 0; direction < 2; ++direction) {
    for (const auto& tensor : direction == 0 ? inputs : outputs) {
      const auto name = tensor["name"].as<std::string>();
      const auto shape = tensor["shape"].as<std::vector<int64_t>>();
      if (!names.insert(name).second || tensor["dtype"].as<std::string>() != "float32" || shape.empty() ||
          shape.size() > 8 || shape[0] != -1)
        throw std::runtime_error("Invalid selected model tensor contract");
      bool present = false;
      for (int i = 0; i < engine.getNbIOTensors(); ++i)
        present = present || name == engine.getIOTensorName(i);
      if (!present ||
          engine.getTensorIOMode(name.c_str()) !=
              (direction == 0 ? nvinfer1::TensorIOMode::kINPUT : nvinfer1::TensorIOMode::kOUTPUT) ||
          engine.getTensorDataType(name.c_str()) != nvinfer1::DataType::kFLOAT ||
          engine.getTensorLocation(name.c_str()) != nvinfer1::TensorLocation::kDEVICE ||
          engine.getTensorFormat(name.c_str()) != nvinfer1::TensorFormat::kLINEAR)
        throw std::runtime_error("Engine binding does not match selected model: " + name);
      const auto dimensions = engine.getTensorShape(name.c_str());
      if (dimensions.nbDims != static_cast<int>(shape.size()))
        throw std::runtime_error("Engine rank does not match selected model: " + name);
      for (int i = 0; i < dimensions.nbDims; ++i)
        if (dimensions.d[i] != shape[i])
          throw std::runtime_error("Engine dimensions do not match selected model: " + name);
      if (direction == 0) {
        for (const auto selector :
             {nvinfer1::OptProfileSelector::kMIN,
              nvinfer1::OptProfileSelector::kOPT,
              nvinfer1::OptProfileSelector::kMAX}) {
          const auto profile = engine.getProfileShape(name.c_str(), 0, selector);
          if (profile.nbDims != dimensions.nbDims || profile.d[0] < 1 || profile.d[0] > args.max_batch ||
              (selector == nvinfer1::OptProfileSelector::kMIN && profile.d[0] != 1) ||
              (selector == nvinfer1::OptProfileSelector::kMAX && profile.d[0] != args.max_batch))
            throw std::runtime_error("Engine batch profile does not match selected model");
          for (int i = 1; i < profile.nbDims; ++i)
            if (profile.d[i] != shape[i])
              throw std::runtime_error("Engine profile dimensions do not match selected model");
        }
      }
    }
  }
}

void Infer(const Arguments& args, Logger& logger, const std::string& identity) {
  auto plan = Read(args.engine, kMaximumEngineBytes);
  std::unique_ptr<nvinfer1::IRuntime> runtime(nvinfer1::createInferRuntime(logger));
  if (!runtime)
    throw std::runtime_error("Cannot create TensorRT runtime");
  std::unique_ptr<nvinfer1::ICudaEngine> engine(runtime->deserializeCudaEngine(plan.data(), plan.size()));
  if (!engine || engine->getNbIOTensors() < 2 || engine->getNbIOTensors() > 9)
    throw std::runtime_error("Invalid engine");
  ValidateContract(*engine, args);
  std::unique_ptr<nvinfer1::IExecutionContext> context(engine->createExecutionContext());
  if (!context)
    throw std::runtime_error("Cannot create execution context");
  std::string input_name;
  for (int i = 0; i < engine->getNbIOTensors(); ++i) {
    const char* name = engine->getIOTensorName(i);
    if (engine->getTensorDataType(name) != nvinfer1::DataType::kFLOAT)
      throw std::runtime_error("Expected float32 bindings");
    if (engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
      if (!input_name.empty())
        throw std::runtime_error("Expected one input");
      input_name = name;
    }
  }
  if (input_name.empty())
    throw std::runtime_error("No engine input");
  auto shape = engine->getTensorShape(input_name.c_str());
  if (shape.nbDims < 2 || shape.d[0] != -1)
    throw std::runtime_error("Expected dynamic batch input");
  shape.d[0] = args.batch;
  if (!context->setInputShape(input_name.c_str(), shape))
    throw std::runtime_error("Batch outside engine profile");
  // Synthetic data is confined to offline preparation; this never reads a video frame.
  const auto host_input = args.contract.empty() ? Read(args.input, kMaximumTensorBytes, TensorBytes(shape))
                                                : std::vector<char>(TensorBytes(shape), 0);
  // Buffers outlive stream destruction, including exceptions during enqueue or transfers.
  std::vector<std::unique_ptr<CudaBuffer>> buffers;
  CudaStream stream;
  for (int i = 0; i < engine->getNbIOTensors(); ++i) {
    const char* name = engine->getIOTensorName(i);
    const auto bytes = TensorBytes(context->getTensorShape(name));
    buffers.emplace_back(std::make_unique<CudaBuffer>(bytes));
    if (!context->setTensorAddress(name, buffers.back()->data()))
      throw std::runtime_error("Cannot bind tensor");
    if (input_name == name)
      Cuda(cudaMemcpyAsync(buffers.back()->data(), host_input.data(), bytes, cudaMemcpyHostToDevice, stream));
  }
  if (!context->enqueueV3(stream))
    throw std::runtime_error("Inference enqueue failed");
  Cuda(cudaStreamSynchronize(stream));
  if (args.contract.empty())
    std::filesystem::create_directories(args.outputs);
  std::ostringstream report;
  report << "{\"runtime\":" << identity << ",\"validated\":" << (args.contract.empty() ? "false" : "true")
         << ",\"batch\":" << args.batch << ",\"outputs\":[";
  bool first = true;
  for (int i = 0; i < engine->getNbIOTensors(); ++i) {
    const char* name = engine->getIOTensorName(i);
    if (engine->getTensorIOMode(name) != nvinfer1::TensorIOMode::kOUTPUT)
      continue;
    shape = context->getTensorShape(name);
    const auto bytes = TensorBytes(shape);
    std::vector<float> values(bytes / sizeof(float));
    Cuda(cudaMemcpy(values.data(), buffers[i]->data(), bytes, cudaMemcpyDeviceToHost));
    if (!std::all_of(values.begin(), values.end(), [](float v) { return std::isfinite(v); }))
      throw std::runtime_error("Nonfinite engine result");
    const auto file = "output_" + std::to_string(i) + ".f32";
    if (args.contract.empty())
      Write(std::filesystem::path(args.outputs) / file, values.data(), bytes);
    if (!first)
      report << ',';
    first = false;
    report << "{\"name\":" << Json(name) << ",\"file\":" << Json(file) << ",\"dtype\":\"float32\",\"shape\":[";
    for (int j = 0; j < shape.nbDims; ++j) {
      if (j)
        report << ',';
      report << shape.d[j];
    }
    report << "]}";
  }
  report << "]}";
  std::cout << report.str() << '\n';
}
} // namespace
int main(int argc, char** argv) {
  try {
    const pid_t parent = ::getppid();
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || ::getppid() != parent)
      throw std::runtime_error("Cannot bind model helper to its parent");
    auto args = Parse(argc, argv);
    if (args.parent_pid && ::getppid() != args.parent_pid)
      throw std::runtime_error("Model preparation parent exited before helper startup");
    const auto identity = RuntimeInfo(args.gpu_id);
    if (args.runtime_info) {
      std::cout << identity << '\n';
      return 0;
    }
    if (!args.tracker_config.empty()) {
      BuildDeepStreamTracker(args.tracker_config, args.tracker_library, args.gpu_id);
      std::cout << "{\"runtime\":" << identity << ",\"tracker_prepared\":true}\n";
      return 0;
    }
    Logger logger;
    if (!args.onnx.empty()) {
      auto plan = Build(args, logger);
      Write(args.engine, plan->data(), plan->size());
      std::cout << "{\"runtime\":" << identity << ",\"engine_bytes\":" << plan->size() << "}\n";
    }
    if (!args.contract.empty()) {
      args.batch = 1;
      Infer(args, logger, identity);
      if (args.max_batch != 1) {
        args.batch = args.max_batch;
        Infer(args, logger, identity);
      }
    } else if (args.onnx.empty()) {
      Infer(args, logger, identity);
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "player-model-builder: " << error.what() << '\n';
    return 1;
  }
}
