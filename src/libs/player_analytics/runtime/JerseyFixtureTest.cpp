#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"
#include "hstream/src/libs/player_analytics/runtime/SemanticRoi.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
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
void Cuda(cudaError_t status) {
  Check(status == cudaSuccess, cudaGetErrorString(status));
}
void Cleanup(cudaError_t status) {
  if (status != cudaSuccess)
    std::cerr << "CUDA cleanup: " << cudaGetErrorString(status) << '\n';
}
struct Buffer {
  void* data{};
  explicit Buffer(size_t size) {
    Cuda(cudaMalloc(&data, size));
  }
  ~Buffer() {
    if (data)
      Cleanup(cudaFree(data));
  }
  template <class T>
  T* as() {
    return static_cast<T*>(data);
  }
};
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
std::vector<float> Read(const fs::path& path, size_t count) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "missing fixture " + path.string());
  std::vector<float> values(count);
  file.read(reinterpret_cast<char*>(values.data()), count * sizeof(float));
  Check(
      file.gcount() == static_cast<std::streamsize>(count * sizeof(float)) && file.peek() == EOF, "wrong fixture size");
  return values;
}
float Maximum(const std::vector<float>& actual, const std::vector<float>& reference) {
  Check(actual.size() == reference.size(), "different output sizes");
  float error = 0;
  for (size_t i = 0; i < actual.size(); ++i) {
    Check(std::isfinite(actual[i]), "nonfinite output");
    error = std::max(error, std::abs(actual[i] - reference[i]));
  }
  return error;
}
int main(int argc, char** argv) {
  try {
    Check(argc == 3, "usage: jersey_fixture_test JERSEY_BUNDLE REAL_JERSEY_FIXTURE");
    const fs::path fixture = argv[2];
    const auto document = YAML::LoadFile((fixture / "fixture.json").string());
    auto bgr = cv::imread((fixture / document["image"].as<std::string>()).string());
    Check(!bgr.empty(), "missing scene");
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    const auto c = document["crop_xywh"];
    const pa::JerseyCrop crop{c[0].as<int>(), c[1].as<int>(), c[2].as<int>(), c[3].as<int>()};
    const auto reference_input = Read(fixture / "input.f32", pa::kJerseyInputElements),
               reference_logits = Read(fixture / "reference.f32", 3 * 95);
    std::vector<float> input(reference_input.size()), logits(reference_logits.size());
    pa::JerseyObservation observation{};
    Cuda(cudaSetDevice(0));
    auto loaded = pa::NativeEngine::Load(argv[1], pa::ModelFeature::kJersey, 0, 1);
    Check(loaded.ok(), loaded.status().ToString());
    auto engine = std::move(*loaded);
    auto vocabulary = pa::MakeJerseyVocabulary(engine->manifest());
    Check(vocabulary.ok(), vocabulary.status().ToString());
    Buffer frame(rgba.step * rgba.rows), scratch(pa::JerseyScratchBytes(1)), output(sizeof(pa::JerseyObservation));
    pa::ImageView image{frame.data, rgba.step, rgba.cols, rgba.rows, pa::PixelFormat::kRgba8};
    Stream stream;
    Cuda(cudaMemcpyAsync(frame.data, rgba.data, rgba.step * rgba.rows, cudaMemcpyHostToDevice, stream.stream));
    Cuda(
        pa::PreprocessJersey(image, &crop, 1, scratch.data, pa::JerseyScratchBytes(1), engine->input(), stream.stream));
    const auto status = engine->Enqueue(1, stream.stream);
    Check(status.ok(), status.ToString());
    Cuda(pa::DecodeJersey(engine->output(0), 1, *vocabulary, output.as<pa::JerseyObservation>(), stream.stream));
    // Pixels/full logits are read back only in this offline reference test.
    Cuda(cudaMemcpyAsync(
        input.data(), engine->input(), input.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaMemcpyAsync(
        logits.data(), engine->output(0), logits.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaMemcpyAsync(&observation, output.data, sizeof(observation), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    const float pre_error = Maximum(input, reference_input), engine_error = Maximum(logits, reference_logits);
    Check(pre_error < .0000002F, "real jersey PIL/torchvision preprocessing parity failed");
    Check(engine_error < .08F, "real jersey PyTorch engine parity failed");
    for (int position = 0; position < 3; ++position) {
      const auto a = logits.cbegin() + position * 95, r = reference_logits.begin() + position * 95;
      Check(
          std::max_element(a, a + 95) - a == std::max_element(r, r + 95) - r,
          "real jersey token argmax differs from PyTorch");
    }
    Check(
        std::string(observation.text) == document["reference"]["text"].as<std::string>(),
        "real jersey decoded text differs from PyTorch");
    const float confidence_delta = std::abs(observation.confidence - document["reference"]["confidence"].as<float>());
    Check(confidence_delta < .01F, "real jersey confidence differs from PyTorch");
    std::cout << "real jersey preprocessing max_abs=" << pre_error << " PyTorch logits max_abs=" << engine_error
              << " text=" << observation.text << " confidence=" << observation.confidence
              << " confidence_delta=" << confidence_delta << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
