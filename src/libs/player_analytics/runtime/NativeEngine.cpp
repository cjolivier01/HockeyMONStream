#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"

#include <hstream_player_tensorrt/NvInfer.h>
#include <hstream_player_tensorrt/NvInferVersion.h>
#include <glib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {
namespace {

constexpr uint64_t kMaximumModelBytes = 1024ULL * 1024 * 1024;
constexpr uint64_t kMaximumManifestBytes = 128 * 1024;

class Failure : public std::runtime_error {
 public:
  Failure(absl::StatusCode code, const std::string& message) : std::runtime_error(message), code(code) {}
  absl::StatusCode code;
};

void Require(bool success, const char* message) {
  if (!success)
    throw Failure(absl::StatusCode::kFailedPrecondition, message);
}

void Cuda(cudaError_t result, const char* operation) {
  if (result != cudaSuccess)
    throw Failure(absl::StatusCode::kInternal, std::string(operation) + ": " + cudaGetErrorString(result));
}

void CleanupCuda(cudaError_t result, const char* operation) noexcept {
  if (result != cudaSuccess)
    std::fprintf(stderr, "player analytics CUDA cleanup %s: %s\n", operation, cudaGetErrorString(result));
}

class File {
 public:
  explicit File(int fd = -1) : fd_(fd) {}
  ~File() {
    if (fd_ >= 0)
      close(fd_);
  }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
  File(File&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  File& operator=(File&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const {
    return fd_;
  }

 private:
  int fd_;
};

File OpenFile(int directory, const std::string& relative) {
  File parent(dup(directory));
  if (parent.get() < 0)
    throw Failure(absl::StatusCode::kInternal, "cannot duplicate bundle directory handle");
  size_t begin = 0;
  for (;;) {
    const auto end = relative.find('/', begin);
    const auto component = relative.substr(begin, end == std::string::npos ? end : end - begin);
    Require(!component.empty() && component != "." && component != "..", "unsafe bundle file component");
    const bool last = end == std::string::npos;
    File opened(openat(parent.get(), component.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? 0 : O_DIRECTORY)));
    if (opened.get() < 0)
      throw Failure(absl::StatusCode::kNotFound, "cannot open regular bundle file: " + relative);
    if (last)
      return opened;
    parent = std::move(opened);
    begin = end + 1;
  }
}

uint64_t FileSize(int fd, uint64_t maximum) {
  struct stat info{};
  Require(fstat(fd, &info) == 0 && S_ISREG(info.st_mode), "bundle file is not regular");
  Require(info.st_size > 0 && static_cast<uint64_t>(info.st_size) <= maximum, "bundle file size exceeds its bound");
  return static_cast<uint64_t>(info.st_size);
}

std::vector<char> ReadFile(int directory, const std::string& name, uint64_t maximum) {
  File file = OpenFile(directory, name);
  std::vector<char> bytes(FileSize(file.get(), maximum));
  size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count = read(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    Require(count > 0, "bundle file changed or failed while reading");
    offset += count;
  }
  char extra;
  Require(read(file.get(), &extra, 1) == 0, "bundle file grew while reading");
  return bytes;
}

void VerifyDigest(const std::vector<char>& bytes, const std::string& expected) {
  gchar* raw =
      g_compute_checksum_for_data(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(bytes.data()), bytes.size());
  std::unique_ptr<gchar, decltype(&g_free)> digest(raw, &g_free);
  Require(digest && expected == digest.get(), "prepared bundle SHA256 mismatch");
}

void VerifyOnnx(int directory, const ModelManifest& manifest) {
  File file = OpenFile(directory, manifest.onnx_file);
  const auto size = FileSize(file.get(), kMaximumModelBytes);
  std::unique_ptr<GChecksum, decltype(&g_checksum_free)> checksum(g_checksum_new(G_CHECKSUM_SHA256), &g_checksum_free);
  Require(static_cast<bool>(checksum), "cannot allocate model checksum");
  std::array<guchar, 64 * 1024> block{};
  uint64_t total = 0;
  for (;;) {
    const ssize_t count = read(file.get(), block.data(), block.size());
    if (count < 0 && errno == EINTR)
      continue;
    Require(count >= 0, "cannot read prepared ONNX provenance file");
    if (!count)
      break;
    total += count;
    Require(total <= size, "prepared ONNX file grew while hashing");
    g_checksum_update(checksum.get(), block.data(), count);
  }
  Require(
      total == size && manifest.onnx_sha256 == g_checksum_get_string(checksum.get()), "prepared ONNX SHA256 mismatch");
}

std::string TrtVersion(int version) {
  return std::to_string(version / 10000) + "." + std::to_string(version / 100 % 100) + "." +
      std::to_string(version % 100);
}

bool ShapeMatches(const nvinfer1::Dims& dims, const TensorContract& contract, int64_t batch = -1) {
  if (dims.nbDims != static_cast<int>(contract.shape.size()))
    return false;
  for (int d = 0; d < dims.nbDims; ++d)
    if (dims.d[d] != (d == 0 ? batch : contract.shape[d]))
      return false;
  return true;
}

size_t ElementsPerSample(const TensorContract& tensor) {
  size_t size = 1;
  for (size_t i = 1; i < tensor.shape.size(); ++i) {
    Require(
        tensor.shape[i] > 0 && static_cast<uint64_t>(tensor.shape[i]) <= kMaximumModelBytes / sizeof(float) / size,
        "tensor size overflow");
    size *= tensor.shape[i];
  }
  return size;
}

class Logger : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* message) noexcept override {
    if (severity > Severity::kERROR)
      return;
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      std::snprintf(error_.data(), error_.size(), "%s", message ? message : "TensorRT error");
    } catch (...) {
    }
  }
  std::string error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_.data();
  }

 private:
  mutable std::mutex mutex_;
  std::array<char, 1024> error_{};
};

struct DeviceBuffer {
  DeviceBuffer() = default;
  explicit DeviceBuffer(size_t elements) {
    Cuda(cudaMalloc(reinterpret_cast<void**>(&data), elements * sizeof(float)), "allocate engine binding");
  }
  ~DeviceBuffer() {
    if (data)
      CleanupCuda(cudaFree(data), "free engine binding");
  }
  DeviceBuffer(const DeviceBuffer&) = delete;
  DeviceBuffer& operator=(const DeviceBuffer&) = delete;
  DeviceBuffer(DeviceBuffer&& other) noexcept : data(std::exchange(other.data, nullptr)) {}
  float* data{nullptr};
};

void ValidateTensor(nvinfer1::ICudaEngine& engine, const TensorContract& contract, nvinfer1::TensorIOMode mode) {
  const char* name = contract.name.c_str();
  Require(engine.getTensorIOMode(name) == mode, "engine input/output name or direction differs from manifest");
  Require(engine.getTensorDataType(name) == nvinfer1::DataType::kFLOAT, "engine binding dtype differs from manifest");
  Require(
      engine.getTensorLocation(name) == nvinfer1::TensorLocation::kDEVICE && !engine.isShapeInferenceIO(name),
      "host and shape inference bindings are unsupported");
  Require(
      engine.getTensorFormat(name) == nvinfer1::TensorFormat::kLINEAR && engine.getTensorVectorizedDim(name) == -1,
      "engine bindings must be unvectorized contiguous tensors");
  Require(ShapeMatches(engine.getTensorShape(name), contract), "engine tensor dimensions differ from manifest");
}

void ValidateResolvedTensor(nvinfer1::IExecutionContext& context, const TensorContract& tensor, size_t batch) {
  Require(
      ShapeMatches(context.getTensorShape(tensor.name.c_str()), tensor, batch),
      "resolved tensor dimensions differ from fixed model contract");
  const auto strides = context.getTensorStrides(tensor.name.c_str());
  Require(strides.nbDims == static_cast<int>(tensor.shape.size()), "resolved tensor stride rank differs");
  int64_t stride = 1;
  for (int dimension = strides.nbDims - 1; dimension >= 0; --dimension) {
    Require(strides.d[dimension] == stride, "resolved tensor binding is not contiguous");
    if (dimension > 0)
      stride *= tensor.shape[dimension];
  }
}

void ValidateProfile(nvinfer1::ICudaEngine& engine, const ModelManifest& manifest) {
  Require(engine.getNbOptimizationProfiles() == 1, "engine must have exactly one fixed batch profile");
  Require(
      engine.getNbIOTensors() == static_cast<int>(manifest.inputs.size() + manifest.outputs.size()),
      "engine tensor count differs from manifest");
  for (const auto& input : manifest.inputs) {
    ValidateTensor(engine, input, nvinfer1::TensorIOMode::kINPUT);
    Require(
        ShapeMatches(engine.getProfileShape(input.name.c_str(), 0, nvinfer1::OptProfileSelector::kMIN), input, 1),
        "engine profile must support minimum batch one");
    Require(
        ShapeMatches(
            engine.getProfileShape(input.name.c_str(), 0, nvinfer1::OptProfileSelector::kMAX),
            input,
            manifest.maximum_batch),
        "engine maximum profile does not match manifest");
    const auto optimum = engine.getProfileShape(input.name.c_str(), 0, nvinfer1::OptProfileSelector::kOPT);
    Require(
        optimum.nbDims > 0 && optimum.d[0] >= 1 && optimum.d[0] <= static_cast<int64_t>(manifest.maximum_batch) &&
            ShapeMatches(optimum, input, optimum.d[0]),
        "engine optimum profile differs from fixed model shape");
  }
  for (const auto& output : manifest.outputs)
    ValidateTensor(engine, output, nvinfer1::TensorIOMode::kOUTPUT);
}

} // namespace

struct NativeEngine::Impl {
  // Declaration order keeps the logger/runtime alive through context teardown.
  Logger logger;
  ModelManifest manifest;
  int gpu_id{0};
  size_t maximum_batch{0};
  size_t last_batch{0};
  std::unique_ptr<nvinfer1::IRuntime> runtime;
  std::unique_ptr<nvinfer1::ICudaEngine> engine;
  std::vector<DeviceBuffer> buffers;
  std::vector<size_t> sample_elements;
  std::unique_ptr<nvinfer1::IExecutionContext> context;

  ~Impl() {
    // The stream-owning caller has already drained all users of these buffers.
    int previous = -1;
    CleanupCuda(cudaGetDevice(&previous), "query teardown device");
    CleanupCuda(cudaSetDevice(gpu_id), "select teardown device");
    context.reset();
    buffers.clear();
    engine.reset();
    runtime.reset();
    if (previous >= 0 && previous != gpu_id)
      CleanupCuda(cudaSetDevice(previous), "restore teardown device");
  }
};

absl::StatusOr<RuntimeIdentity> QueryRuntimeIdentity(int gpu_id) {
  try {
    const int version = getInferLibVersion();
    Require(
        version == NV_TENSORRT_MAJOR * 10000 + NV_TENSORRT_MINOR * 100 + NV_TENSORRT_PATCH &&
            getInferLibBuildVersion() == NV_TENSORRT_BUILD,
        "TensorRT headers and loaded runtime differ");
    cudaDeviceProp device{};
    Cuda(cudaGetDeviceProperties(&device, gpu_id), "query GPU identity");
    RuntimeIdentity identity;
    identity.tensorrt_version = TrtVersion(version);
    identity.tensorrt_build_version = getInferLibBuildVersion();
    Cuda(cudaRuntimeGetVersion(&identity.cuda_runtime_version), "query CUDA runtime identity");
    identity.gpu_name = device.name;
    identity.compute_capability = std::to_string(device.major) + "." + std::to_string(device.minor);
    return identity;
  } catch (const Failure& error) {
    return absl::Status(error.code, error.what());
  } catch (const std::exception& error) {
    return absl::InternalError(error.what());
  }
}

NativeEngine::NativeEngine(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
NativeEngine::~NativeEngine() = default;

absl::StatusOr<std::unique_ptr<NativeEngine>> NativeEngine::Load(
    const std::filesystem::path& directory,
    ModelFeature expected,
    int gpu_id,
    size_t required_batch) {
  try {
    if (gpu_id < 0 || required_batch == 0 || required_batch > kMaximumBatch)
      return absl::InvalidArgumentError("invalid GPU or configured analytics batch bound");
    File root(open(directory.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY));
    if (root.get() < 0)
      return absl::NotFoundError("cannot open prepared player model bundle directory: " + directory.string());
    const auto document = ReadFile(root.get(), "manifest.json", kMaximumManifestBytes);
    auto manifest = ParseModelManifest(YAML::Load(std::string(document.begin(), document.end())));
    if (!manifest.ok())
      return manifest.status();
    Require(manifest->feature == expected, "prepared bundle feature differs from requested model");
    Require(required_batch <= manifest->maximum_batch, "prepared model does not support the configured batch size");
    VerifyOnnx(root.get(), *manifest);
    auto serialized = ReadFile(root.get(), manifest->engine_file, kMaximumModelBytes);
    VerifyDigest(serialized, manifest->engine_sha256);
    auto identity = QueryRuntimeIdentity(gpu_id);
    if (!identity.ok())
      return identity.status();
    const auto identity_status = ValidateRuntimeIdentity(*manifest, *identity);
    if (!identity_status.ok())
      return identity_status;
    Cuda(cudaSetDevice(gpu_id), "select inference GPU");
    auto impl = std::make_unique<Impl>();
    impl->manifest = std::move(*manifest);
    impl->gpu_id = gpu_id;
    impl->maximum_batch = required_batch;
    impl->runtime.reset(nvinfer1::createInferRuntime(impl->logger));
    Require(static_cast<bool>(impl->runtime), "cannot create native TensorRT runtime");
    impl->engine.reset(impl->runtime->deserializeCudaEngine(serialized.data(), serialized.size()));
    if (!impl->engine)
      throw Failure(
          absl::StatusCode::kFailedPrecondition, "cannot deserialize prepared engine: " + impl->logger.error());
    ValidateProfile(*impl->engine, impl->manifest);
    impl->context.reset(impl->engine->createExecutionContext());
    Require(static_cast<bool>(impl->context), "cannot create TensorRT execution context");
    auto input_shape = impl->engine->getTensorShape(impl->manifest.inputs[0].name.c_str());
    input_shape.d[0] = required_batch;
    Require(
        impl->context->setInputShape(impl->manifest.inputs[0].name.c_str(), input_shape),
        "cannot set model batch shape");
    const size_t tensor_count = impl->manifest.inputs.size() + impl->manifest.outputs.size();
    impl->buffers.reserve(tensor_count);
    impl->sample_elements.reserve(tensor_count);
    for (const auto* contracts : {&impl->manifest.inputs, &impl->manifest.outputs}) {
      for (const auto& contract : *contracts) {
        ValidateResolvedTensor(*impl->context, contract, required_batch);
        const size_t per_sample = ElementsPerSample(contract);
        Require(
            per_sample <= kMaximumModelBytes / sizeof(float) / required_batch, "binding allocation exceeds hard bound");
        impl->buffers.emplace_back(per_sample * required_batch);
        impl->sample_elements.push_back(per_sample);
        Require(
            impl->context->setTensorAddress(contract.name.c_str(), impl->buffers.back().data),
            "cannot bind device tensor");
      }
    }
    Require(impl->context->inferShapes(0, nullptr) == 0, "model shapes require unsupported extra inputs");
    impl->last_batch = required_batch;
    return std::unique_ptr<NativeEngine>(new NativeEngine(std::move(impl)));
  } catch (const Failure& error) {
    return absl::Status(error.code, error.what());
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("load player model: ") + error.what());
  }
}

const ModelManifest& NativeEngine::manifest() const noexcept {
  return impl_->manifest;
}
int NativeEngine::gpu_id() const noexcept {
  return impl_->gpu_id;
}
size_t NativeEngine::maximum_batch() const noexcept {
  return impl_->maximum_batch;
}
float* NativeEngine::input() noexcept {
  return impl_->buffers[0].data;
}
const float* NativeEngine::output(size_t index) const noexcept {
  return index < impl_->manifest.outputs.size() ? impl_->buffers[index + 1].data : nullptr;
}
size_t NativeEngine::input_elements_per_sample() const noexcept {
  return impl_->sample_elements[0];
}
size_t NativeEngine::output_elements_per_sample(size_t index) const noexcept {
  return index < impl_->manifest.outputs.size() ? impl_->sample_elements[index + 1] : 0;
}

absl::Status NativeEngine::Enqueue(size_t batch, cudaStream_t stream) {
  try {
    if (!stream || batch == 0 || batch > impl_->maximum_batch)
      return absl::InvalidArgumentError("invalid inference stream or batch size");
    int device = -1;
    Cuda(cudaGetDevice(&device), "query inference device");
    Require(device == impl_->gpu_id, "caller selected a different GPU from the prepared execution context");
    if (batch != impl_->last_batch) {
      auto dimensions = impl_->engine->getTensorShape(impl_->manifest.inputs[0].name.c_str());
      dimensions.d[0] = batch;
      Require(
          impl_->context->setInputShape(impl_->manifest.inputs[0].name.c_str(), dimensions),
          "cannot select inference batch");
      for (const auto* tensors : {&impl_->manifest.inputs, &impl_->manifest.outputs})
        for (const auto& tensor : *tensors)
          ValidateResolvedTensor(*impl_->context, tensor, batch);
      impl_->last_batch = batch;
    }
    if (!impl_->context->enqueueV3(stream))
      return absl::InternalError("player model enqueue failed: " + impl_->logger.error());
    return absl::OkStatus();
  } catch (const Failure& error) {
    return absl::Status(error.code, error.what());
  } catch (const std::exception& error) {
    return absl::InternalError(std::string("enqueue player model: ") + error.what());
  }
}

} // namespace hm::player_analytics
