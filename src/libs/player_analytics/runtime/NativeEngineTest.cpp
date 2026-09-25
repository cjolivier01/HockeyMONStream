#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace pa = hm::player_analytics;
namespace fs = std::filesystem;

void Check(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t result) {
  Check(result == cudaSuccess, cudaGetErrorString(result));
}
void Cleanup(cudaError_t result) {
  if (result != cudaSuccess)
    std::cerr << "CUDA cleanup: " << cudaGetErrorString(result) << '\n';
}
struct Stream {
  cudaStream_t stream{};
  Stream() {
    Cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  }
  ~Stream() {
    Cleanup(cudaStreamSynchronize(stream));
    Cleanup(cudaStreamDestroy(stream));
  }
};
std::vector<float> Read(const fs::path& path, size_t elements) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "missing reference fixture: " + path.string());
  std::vector<float> values(elements);
  file.read(reinterpret_cast<char*>(values.data()), elements * sizeof(float));
  Check(
      file.gcount() == static_cast<std::streamsize>(elements * sizeof(float)) && file.peek() == EOF,
      "unexpected fixture size: " + path.string());
  return values;
}
struct PrivateBundle {
  fs::path path;
  YAML::Node manifest;
  explicit PrivateBundle(const fs::path& original) {
    char name[] = "/tmp/hstream-native-engine-test-XXXXXX";
    Check(mkdtemp(name) != nullptr, "cannot create private test bundle");
    path = name;
    manifest = YAML::LoadFile((original / "manifest.json").string());
    for (const char* kind : {"onnx", "engine"}) {
      const auto relative = manifest[kind]["file"].as<std::string>();
      fs::create_directories((path / relative).parent_path());
      fs::create_hard_link(original / relative, path / relative);
    }
    Save();
  }
  ~PrivateBundle() {
    std::error_code error;
    fs::remove_all(path, error);
  }
  void Save() {
    std::ofstream(path / "manifest.json") << manifest;
  }
  void Rejected(const char* reason, size_t batch = 8) {
    Save();
    const auto result = pa::NativeEngine::Load(path, pa::ModelFeature::kPose, 0, batch);
    Check(!result.ok(), std::string("invalid prepared bundle accepted: ") + reason);
    std::cout << "rejected " << reason << ": " << result.status() << '\n';
  }
};

int main(int argc, char** argv) {
  try {
    Check(argc == 3, "usage: native_engine_test POSE_BUNDLE POSE_EXPORT_FIXTURES");
    const fs::path bundle = argv[1], fixtures = argv[2];
    Cuda(cudaSetDevice(0));
    auto loaded = pa::NativeEngine::Load(bundle, pa::ModelFeature::kPose, 0, 8);
    Check(loaded.ok(), loaded.status().ToString());
    auto engine = std::move(*loaded);
    Stream stream; // Retire all submitted work before context/bindings, including exceptions.
    Check(
        engine->maximum_batch() == 8 && engine->input_elements_per_sample() == 3 * 256 * 192 &&
            engine->output_elements_per_sample(0) == 17 * 384 && engine->output_elements_per_sample(1) == 17 * 512 &&
            engine->output(2) == nullptr && engine->output_elements_per_sample(2) == 0,
        "binding capacities differ");
    for (size_t batch : {1U, 2U, 8U, 1U}) {
      auto input =
          Read(fixtures / ("input_b" + std::to_string(batch) + ".f32"), batch * engine->input_elements_per_sample());
      Cuda(cudaMemcpyAsync(
          engine->input(), input.data(), input.size() * sizeof(float), cudaMemcpyHostToDevice, stream.stream));
      const auto status = engine->Enqueue(batch, stream.stream);
      Check(status.ok(), status.ToString());
      for (size_t output = 0; output < 2; ++output) {
        const size_t elements = batch * engine->output_elements_per_sample(output);
        auto reference =
            Read(fixtures / ("reference_b" + std::to_string(batch) + "_" + std::to_string(output) + ".f32"), elements);
        std::vector<float> actual(elements);
        Cuda(cudaMemcpyAsync(
            actual.data(), engine->output(output), elements * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
        Cuda(cudaStreamSynchronize(stream.stream));
        float maximum = 0;
        for (size_t i = 0; i < elements; ++i) {
          Check(std::isfinite(actual[i]), "nonfinite inference output");
          maximum = std::max(maximum, std::abs(actual[i] - reference[i]));
        }
        std::cout << "batch=" << batch << " output=" << output << " PyTorch max_abs=" << maximum << '\n';
        Check(maximum < 0.01F, "FP16 engine diverges from PyTorch reference");
      }
    }
    Check(
        !engine->Enqueue(0, stream.stream).ok() && !engine->Enqueue(9, stream.stream).ok() &&
            !engine->Enqueue(1, nullptr).ok(),
        "invalid batch/stream was accepted");
    Check(!pa::NativeEngine::Load(bundle, pa::ModelFeature::kJersey, 0, 1).ok(), "wrong feature accepted");
    Check(!pa::NativeEngine::Load(bundle, pa::ModelFeature::kPose, 0, 9).ok(), "batch beyond hard cap accepted");
    {
      auto bounded = pa::NativeEngine::Load(bundle, pa::ModelFeature::kPose, 0, 2);
      Check(
          bounded.ok() && (*bounded)->maximum_batch() == 2 && !(*bounded)->Enqueue(3, stream.stream).ok(),
          "configured capacity was not enforced");
    }
    {
      PrivateBundle b(bundle);
      b.manifest["engine"]["sha256"] = std::string(64, '0');
      b.Rejected("engine hash");
    }
    {
      PrivateBundle b(bundle);
      b.manifest["onnx"]["sha256"] = std::string(64, '0');
      b.Rejected("ONNX hash");
    }
    {
      PrivateBundle b(bundle);
      b.manifest["engine"]["gpu_name"] = "wrong GPU";
      b.Rejected("runtime identity");
    }
    {
      PrivateBundle b(bundle);
      b.manifest["inputs"][0]["name"] = "wrong_input";
      b.Rejected("actual engine binding name");
    }
    {
      PrivateBundle b(bundle);
      b.manifest["inputs"][0]["shape"][2] = 255;
      b.Rejected("fixed input shape");
    }
    {
      PrivateBundle b(bundle);
      b.manifest["engine"]["max_batch"] = 4;
      b.Rejected("configured batch exceeds manifest");
      b.Rejected("actual engine profile", 4);
    }
    {
      PrivateBundle b(bundle);
      const auto name = b.manifest["engine"]["file"].as<std::string>();
      fs::remove(b.path / name);
      b.Rejected("missing engine");
      fs::create_symlink(bundle / name, b.path / name);
      b.Rejected("symlink engine escape");
    }
    {
      PrivateBundle b(bundle);
      fs::remove(b.path / "manifest.json");
      fs::create_symlink(bundle / "manifest.json", b.path / "manifest.json");
      Check(!pa::NativeEngine::Load(b.path, pa::ModelFeature::kPose, 0, 1).ok(), "symlink manifest accepted");
    }
    std::cout << "native engine validation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
