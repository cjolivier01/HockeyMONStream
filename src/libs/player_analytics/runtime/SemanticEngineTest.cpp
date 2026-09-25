#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"
#include "hstream/src/libs/player_analytics/runtime/SemanticRoi.h"

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
void Cuda(cudaError_t result) {
  Check(result == cudaSuccess, cudaGetErrorString(result));
}
void Cleanup(cudaError_t result) {
  if (result != cudaSuccess)
    std::cerr << "CUDA cleanup: " << cudaGetErrorString(result) << '\n';
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
struct Top {
  int label;
  double probability;
};
Top Softmax(const float* data, size_t count) {
  const size_t label = std::max_element(data, data + count) - data;
  double sum = 0;
  for (size_t i = 0; i < count; ++i) {
    Check(std::isfinite(data[i]), "nonfinite logits");
    sum += std::exp(static_cast<double>(data[i]) - data[label]);
  }
  return {static_cast<int>(label), 1 / sum};
}
pa::JerseyObservation Jersey(const float* logits, const pa::JerseyVocabulary& vocabulary) {
  pa::JerseyObservation result;
  double probability = 1;
  for (int position = 0; position < 3; ++position) {
    const auto top = Softmax(logits + position * 95, 95);
    probability *= top.probability;
    if (top.label == vocabulary.eos_index) {
      if (position > 0)
        result.confidence = probability;
      return result;
    }
    if (vocabulary.digits[top.label] < 0 || position == 2)
      return {};
    result.text[position] = '0' + vocabulary.digits[top.label];
    result.length = position + 1;
  }
  return {};
}
void Run(const fs::path& bundle, const fs::path& fixtures, pa::ModelFeature feature) {
  const bool jersey = feature == pa::ModelFeature::kJersey;
  auto loaded = pa::NativeEngine::Load(bundle, feature, 0, 8);
  Check(loaded.ok(), loaded.status().ToString());
  auto engine = std::move(*loaded);
  pa::JerseyVocabulary vocabulary{};
  if (jersey) {
    auto mapped = pa::MakeJerseyVocabulary(engine->manifest());
    Check(mapped.ok(), mapped.status().ToString());
    vocabulary = *mapped;
  }
  const size_t count = engine->output_elements_per_sample(0);
  std::vector<float> input(engine->input_elements_per_sample() * 8), actual(count * 8), reference(count * 8);
  std::array<pa::JerseyObservation, 8> jerseys{};
  std::array<pa::ActionObservation, 8> actions{};
  Buffer results(std::max(sizeof(jerseys), sizeof(actions)));
  Stream stream;
  for (size_t batch : {1U, 2U, 8U, 1U}) {
    input = Read(fixtures / ("input_b" + std::to_string(batch) + ".f32"), engine->input_elements_per_sample() * batch);
    reference = Read(fixtures / ("reference_b" + std::to_string(batch) + "_0.f32"), count * batch);
    Cuda(cudaMemcpyAsync(
        engine->input(), input.data(), input.size() * sizeof(float), cudaMemcpyHostToDevice, stream.stream));
    const auto status = engine->Enqueue(batch, stream.stream);
    Check(status.ok(), status.ToString());
    if (jersey)
      Cuda(pa::DecodeJersey(engine->output(0), batch, vocabulary, results.as<pa::JerseyObservation>(), stream.stream));
    else
      Cuda(pa::ReduceAction(engine->output(0), batch, results.as<pa::ActionObservation>(), stream.stream));
    Cuda(cudaMemcpyAsync(
        actual.data(), engine->output(0), reference.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaMemcpyAsync(
        jersey ? static_cast<void*>(jerseys.data()) : static_cast<void*>(actions.data()),
        results.data,
        batch * (jersey ? sizeof(pa::JerseyObservation) : sizeof(pa::ActionObservation)),
        cudaMemcpyDeviceToHost,
        stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    float maximum = 0;
    double maximum_confidence = 0;
    for (size_t i = 0; i < reference.size(); ++i) {
      Check(std::isfinite(actual[i]), "nonfinite engine output");
      maximum = std::max(maximum, std::abs(actual[i] - reference[i]));
    }
    for (size_t b = 0; b < batch; ++b) {
      if (jersey) {
        const auto cpu = Jersey(actual.data() + b * count, vocabulary),
                   pytorch = Jersey(reference.data() + b * count, vocabulary);
        Check(
            std::string(cpu.text) == jerseys[b].text && cpu.length == jerseys[b].length &&
                std::abs(cpu.confidence - jerseys[b].confidence) < .000001F,
            "compact PARSeq decoder diverged from engine logits");
        Check(std::string(cpu.text) == pytorch.text, "PARSeq greedy result differs from PyTorch");
        maximum_confidence =
            std::max(maximum_confidence, static_cast<double>(std::abs(jerseys[b].confidence - pytorch.confidence)));
        for (int position = 0; position < 3; ++position)
          Check(
              Softmax(actual.data() + b * count + position * 95, 95).label ==
                  Softmax(reference.data() + b * count + position * 95, 95).label,
              "PARSeq token argmax differs from PyTorch");
      } else {
        const auto cpu = Softmax(actual.data() + b * count, count),
                   pytorch = Softmax(reference.data() + b * count, count);
        Check(
            cpu.label == actions[b].label && std::abs(cpu.probability - actions[b].confidence) < .000001,
            "compact action softmax differs from engine logits");
        Check(cpu.label == pytorch.label, "action label differs from PyTorch");
        maximum_confidence = std::max(maximum_confidence, std::abs(actions[b].confidence - pytorch.probability));
      }
    }
    std::cout << (jersey ? "jersey" : "action") << " batch=" << batch << " PyTorch max_abs=" << maximum
              << " confidence_delta=" << maximum_confidence << '\n';
    Check(maximum < (jersey ? .04F : .02F) && maximum_confidence < .01, "prepared semantic engine parity failed");
  }
}
int main(int argc, char** argv) {
  try {
    Check(argc == 5, "usage: semantic_engine_test JERSEY_BUNDLE JERSEY_EXPORT ACTION_BUNDLE ACTION_EXPORT");
    Cuda(cudaSetDevice(0));
    Run(argv[1], argv[2], pa::ModelFeature::kJersey);
    Run(argv[3], argv[4], pa::ModelFeature::kAction);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
