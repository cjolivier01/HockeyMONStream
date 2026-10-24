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

void TestCompute(const Model& model, Session::Precision precision) {
  Stream stream, other;
  constexpr size_t kIterations = 16;
  Host inputs(kIterations * 4), outputs(kIterations * 2);
  auto created = Session::Compile(model.path, {{"left", {1, 2}}, {"right", {1, 2}}}, precision, 0, 24);
  Check(created.ok(), created.status().ToString());
  auto session = std::move(*created);
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
} // namespace
int main() {
  try {
    Model add(kAddModel, sizeof(kAddModel)), identity(kIdentityModel, sizeof(kIdentityModel));
    TestCompute(add, Session::Precision::kFloat32);
    TestCompute(add, Session::Precision::kFloat16);
    TestIdentity(identity);
    TestInvalid(add);
    std::cout << "MIGraphX GPU inference, stream ordering, lifetime and contract checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
