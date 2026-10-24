#include "hstream/src/libs/gpu/migraphx/MigraphxSession.h"

#include <unistd.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using Session = hm::gpu::MigraphxSession;
void Check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Hip(hipError_t result) {
  Check(result == hipSuccess, hipGetErrorString(result));
}
struct Stream {
  hipStream_t value{};
  Stream() {
    Hip(hipStreamCreateWithFlags(&value, hipStreamNonBlocking));
  }
  ~Stream() {
    (void)hipStreamDestroy(value);
  }
};
struct Host {
  float* data{};
  explicit Host(size_t elements) {
    Hip(hipHostMalloc(reinterpret_cast<void**>(&data), elements * sizeof(float)));
  }
  ~Host() {
    (void)hipHostFree(data);
  }
};
struct Model {
  std::string path;
  Model(const unsigned char* bytes, size_t size) {
    path = (std::filesystem::temp_directory_path() / "hstream-migraphx-XXXXXX").string();
    const int fd = mkstemp(path.data());
    Check(fd >= 0, "create temporary model");
    close(fd);
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(bytes), size);
    Check(static_cast<bool>(file), "write temporary model");
  }
  ~Model() {
    std::filesystem::remove(path);
  }
};
constexpr unsigned char kIdentityModel[] = {
    0x08, 0x09, 0x3a, 0x58, 0x0a, 0x19, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x06, 0x6f, 0x75,
    0x74, 0x70, 0x75, 0x74, 0x22, 0x08, 0x49, 0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x12, 0x08, 0x69,
    0x64, 0x65, 0x6e, 0x74, 0x69, 0x74, 0x79, 0x5a, 0x17, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12,
    0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x18,
    0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a,
    0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};

// A two-input Add graph with float32 [1, 2] inputs. Keeping this tiny fixture
// in the test exercises the same multi-input Session path used by LightGlue
// without checking a trained model into the repository.
constexpr unsigned char kAddModel[] = {
    0x08, 0x09, 0x3a, 0x71, 0x0a, 0x1f, 0x0a, 0x04, 0x6c, 0x65, 0x66, 0x74, 0x0a, 0x05, 0x72, 0x69, 0x67, 0x68,
    0x74, 0x12, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x1a, 0x03, 0x61, 0x64, 0x64, 0x22, 0x03, 0x41, 0x64,
    0x64, 0x12, 0x03, 0x61, 0x64, 0x64, 0x5a, 0x16, 0x0a, 0x04, 0x6c, 0x65, 0x66, 0x74, 0x12, 0x0e, 0x0a, 0x0c,
    0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x5a, 0x17, 0x0a, 0x05, 0x72, 0x69,
    0x67, 0x68, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08,
    0x02, 0x62, 0x18, 0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12,
    0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};

void TestCompute(
    const Model& model,
    Session::Precision precision,
    const std::optional<hm::gpu::MigraphxCacheConfig>& cache = std::nullopt,
    bool hit = false) {
  Stream stream, other;
  constexpr size_t kIterations = 16;
  Host inputs(kIterations * 4), outputs(kIterations * 2);
  auto created = Session::Compile(model.path, {{"left", {1, 2}}, {"right", {1, 2}}}, precision, 0, 24, cache);
  Check(created.ok(), created.status().ToString());
  auto session = std::move(*created);
  Check(session->cache_hit() == hit, "unexpected cache hit/miss");
  Check(session->input_count() == 2 && session->output_count() == 1, "input/output count");
  Check(session->input(0).dimensions == std::vector<size_t>({1, 2}), "input shape");
  Check(session->output(0).data == nullptr, "no output before enqueue");
  Check(!session->Enqueue(nullptr).ok(), "reject default stream");
  Check(!session->Enqueue(hipStreamPerThread).ok(), "reject implicit thread stream");
  for (size_t i = 0; i < kIterations; ++i) {
    float* values = inputs.data + 4 * i;
    values[0] = i + 0.25f;
    values[1] = -static_cast<float>(i) - 0.5f;
    values[2] = 2 * i;
    values[3] = 4;
    Hip(hipMemcpyAsync(session->input(0).data, values, 8, hipMemcpyHostToDevice, stream.value));
    Hip(hipMemcpyAsync(session->input(1).data, values + 2, 8, hipMemcpyHostToDevice, stream.value));
    const auto status = session->Enqueue(stream.value);
    Check(status.ok(), status.ToString());
    Hip(hipMemcpyAsync(outputs.data + 2 * i, session->output(0).data, 8, hipMemcpyDeviceToHost, stream.value));
  }
  Check(!session->Enqueue(other.value).ok(), "reject switching an active session to another stream");
  // No explicit wait: session teardown must finish every queued output consumer.
  session.reset();
  for (size_t i = 0; i < kIterations; ++i) {
    Check(outputs.data[2 * i] == 3 * i + 0.25f, "ordered first output");
    Check(outputs.data[2 * i + 1] == 3.5f - i, "ordered second output");
  }
  Hip(hipStreamQuery(stream.value));
  Hip(hipMemsetAsync(inputs.data, 0, 16, stream.value));
  Hip(hipStreamSynchronize(stream.value));
}
void TestIdentity(const Model& model) {
  Stream stream;
  Host input(2), output(2);
  input.data[0] = -3.5f;
  input.data[1] = 7.25f;
  auto created = Session::Compile(model.path, {{"input", {1, 2}}}, Session::Precision::kFloat32, 0, 16);
  Check(created.ok(), created.status().ToString());
  auto session = std::move(*created);
  Hip(hipMemcpyAsync(session->input(0).data, input.data, 8, hipMemcpyHostToDevice, stream.value));
  const auto status = session->Enqueue(stream.value);
  Check(status.ok(), status.ToString());
  Hip(hipMemcpyAsync(output.data, session->output(0).data, 8, hipMemcpyDeviceToHost, stream.value));
  session.reset();
  Check(output.data[0] == input.data[0] && output.data[1] == input.data[1], "identity aliases remain live");
}
void TestInvalid(const Model& model) {
  using Shape = hm::gpu::ModelInputShape;
  const std::vector<Shape> good{{"left", {1, 2}}, {"right", {1, 2}}};
  const auto compile = [&](const std::vector<Shape>& shapes, size_t bytes) {
    return Session::Compile(model.path, shapes, Session::Precision::kFloat32, 0, bytes);
  };
  Check(!compile({}, 24).ok(), "no inputs");
  Check(!compile({{"left", {1, 2}}}, 24).ok(), "missing input");
  Check(!compile({{"wrong", {1, 2}}, {"right", {1, 2}}}, 24).ok(), "unknown input");
  Check(!compile({{"left", {1, 2}}, {"left", {1, 2}}}, 24).ok(), "duplicate input");
  Check(!compile({{"left", {0, 2}}, {"right", {1, 2}}}, 24).ok(), "zero axis");
  Check(!compile({{"left", {std::numeric_limits<size_t>::max(), 2}}}, 24).ok(), "shape overflow");
  Check(!compile(good, 23).ok(), "aggregate binding bound");
  Check(!Session::Compile(model.path + ".missing", good, Session::Precision::kFloat32, 0, 24).ok(), "missing model");
  Check(!Session::Compile(model.path, good, Session::Precision::kFloat32, -1, 24).ok(), "invalid device");
}
struct Directory {
  std::filesystem::path path;
  Directory() {
    std::string value = (std::filesystem::temp_directory_path() / "hstream-cache-test-XXXXXX").string();
    Check(mkdtemp(value.data()), "create cache test directory");
    path = value;
  }
  ~Directory() {
    std::filesystem::remove_all(path);
  }
};
void TestCache(const Model& add) {
  Directory directory;
  const hm::gpu::MigraphxCacheConfig cache{directory.path / "cache", 1048576};
  TestCompute(add, Session::Precision::kFloat32, cache, false);
  TestCompute(add, Session::Precision::kFloat32, cache, true);
  TestCompute(add, Session::Precision::kFloat16, cache, false);
  TestCompute(add, Session::Precision::kFloat16, cache, true);
  const auto compile = [&](const hm::gpu::MigraphxCacheConfig& config, size_t io) {
    return Session::Compile(
        add.path, {{"left", {1, 2}}, {"right", {1, 2}}}, Session::Precision::kFloat32, 0, io, config);
  };
  Check(!compile(cache, 23).ok(), "cached graph bypassed binding bound");
  Check(!compile({cache.directory, 1}, 24).ok(), "cache byte bound ignored");
  Check(!compile({"relative", cache.maximum_entry_bytes}, 24).ok(), "relative cache accepted");
  std::filesystem::create_directory_symlink(cache.directory, directory.path / "alias");
  Check(!compile({directory.path / "alias", cache.maximum_entry_bytes}, 24).ok(), "cache symlink accepted");
  setenv("MIGRAPHX_TRACE_COMPILE", "0", 1);
  TestCompute(add, Session::Precision::kFloat32, cache, false);
  unsetenv("MIGRAPHX_TRACE_COMPILE");
  TestCompute(add, Session::Precision::kFloat32, cache, true);
  size_t entries = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cache.directory)) {
    Check(entry.path().filename().string().size() == 64, "temporary preparation leaked");
    ++entries;
    std::ofstream program(entry.path() / "program.mxr", std::ios::binary | std::ios::trunc);
    program << "corrupt";
  }
  Check(entries == 3, "precision/environment did not isolate cache entries");
  const auto corrupt = compile(cache, 24);
  Check(
      !corrupt.ok() && corrupt.status().ToString().find("corrupt entry") != std::string::npos,
      "corrupt cache silently recompiled");
}

// Add(input, weight) with an external float32 [1,2] initializer. Replacing only
// weights.bin must invalidate the cache and alter the computed result.
constexpr unsigned char kExternalModel[] = {
    0x08, 0x09, 0x3a, 0xa0, 0x01, 0x0a, 0x1c, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x0a, 0x06, 0x77, 0x65, 0x69,
    0x67, 0x68, 0x74, 0x12, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x22, 0x03, 0x41, 0x64, 0x64, 0x12, 0x08, 0x65,
    0x78, 0x74, 0x65, 0x72, 0x6e, 0x61, 0x6c, 0x2a, 0x43, 0x08, 0x01, 0x08, 0x02, 0x10, 0x01, 0x42, 0x06, 0x77, 0x65,
    0x69, 0x67, 0x68, 0x74, 0x6a, 0x17, 0x0a, 0x08, 0x6c, 0x6f, 0x63, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x12, 0x0b, 0x77,
    0x65, 0x69, 0x67, 0x68, 0x74, 0x73, 0x2e, 0x62, 0x69, 0x6e, 0x6a, 0x0b, 0x0a, 0x06, 0x6f, 0x66, 0x66, 0x73, 0x65,
    0x74, 0x12, 0x01, 0x30, 0x6a, 0x0b, 0x0a, 0x06, 0x6c, 0x65, 0x6e, 0x67, 0x74, 0x68, 0x12, 0x01, 0x38, 0x70, 0x01,
    0x5a, 0x17, 0x0a, 0x05, 0x69, 0x6e, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a, 0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02,
    0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x62, 0x18, 0x0a, 0x06, 0x6f, 0x75, 0x74, 0x70, 0x75, 0x74, 0x12, 0x0e, 0x0a,
    0x0c, 0x08, 0x01, 0x12, 0x08, 0x0a, 0x02, 0x08, 0x01, 0x0a, 0x02, 0x08, 0x02, 0x42, 0x04, 0x0a, 0x00, 0x10, 0x11,
};
void TestExternalCache() {
  Directory directory;
  Model external(kExternalModel, sizeof(kExternalModel));
  const auto model = directory.path / "model.onnx";
  std::filesystem::copy_file(external.path, model);
  const hm::gpu::MigraphxCacheConfig cache{directory.path / "cache", 1048576};
  Stream stream;
  Host output(2);
  for (int iteration = 0; iteration < 4; ++iteration) {
    const float weights[2] = {2.f + 10 * (iteration / 2), 4.f};
    {
      std::ofstream data(directory.path / "weights.bin", std::ios::binary | std::ios::trunc);
      data.write(reinterpret_cast<const char*>(weights), sizeof(weights));
    }
    auto created = Session::Compile(model, {{"input", {1, 2}}}, Session::Precision::kFloat32, 0, 16, cache);
    Check(created.ok(), created.status().ToString());
    auto session = std::move(*created);
    Check(session->cache_hit() == bool(iteration % 2), "external weights cache invalidation/reuse");
    Hip(hipMemsetAsync(session->input(0).data, 0, 8, stream.value));
    Check(session->Enqueue(stream.value).ok(), "enqueue external graph");
    Hip(hipMemcpyAsync(output.data, session->output(0).data, 8, hipMemcpyDeviceToHost, stream.value));
    session.reset();
    Check(output.data[0] == weights[0] && output.data[1] == weights[1], "cached external weights changed result");
  }
}
} // namespace
int main() {
  try {
    Model add(kAddModel, sizeof(kAddModel)), identity(kIdentityModel, sizeof(kIdentityModel));
    TestCompute(add, Session::Precision::kFloat32);
    TestCompute(add, Session::Precision::kFloat16);
    TestIdentity(identity);
    TestInvalid(add);
    TestCache(add);
    TestExternalCache();
    std::cout << "MIGraphX GPU inference, stream ordering, lifetime and contract checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
