#include "hstream/src/libs/player_analytics/runtime/SemanticRoi.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
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
template <class T>
std::vector<T> Read(const fs::path& path, size_t count) {
  std::ifstream file(path, std::ios::binary);
  Check(file.good(), "missing fixture " + path.string());
  std::vector<T> result(count);
  file.read(reinterpret_cast<char*>(result.data()), count * sizeof(T));
  Check(file.gcount() == static_cast<std::streamsize>(count * sizeof(T)) && file.peek() == EOF, "wrong fixture size");
  return result;
}

void RoiTests() {
  pa::ImageView view{nullptr, 0, 640, 480, pa::PixelFormat::kRgba8};
  auto bbox = pa::MakeJerseyCrop(view, {10, 20, 100, 200}, 320, 960, pa::JerseyRoiMode::kBox);
  Check(
      bbox && bbox.crop.left == 60 && bbox.crop.top == 35 && bbox.crop.width == 120 && bbox.crop.height == 70,
      "bbox torso fractions/nonuniform mapping failed");
  pa::Pose pose{};
  pose[5] = {30, 60, .8F};
  pose[6] = {70, 60, .8F};
  pose[11] = {35, 120, .8F};
  pose[12] = {65, 120, .8F};
  auto torso = pa::MakeJerseyCrop(view, {10, 20, 100, 200}, 320, 960, pa::JerseyRoiMode::kPose, &pose);
  Check(
      torso && torso.crop.left == 56 && torso.crop.top == 28 && torso.crop.width == 88 && torso.crop.height == 34,
      "pose torso bounds/relative padding failed");
  pose[5].confidence = .399F;
  Check(
      pa::MakeJerseyCrop(view, {10, 20, 100, 200}, 320, 960, pa::JerseyRoiMode::kPose, &pose).reason ==
          pa::JerseyCropReason::kInsufficientPose,
      "pose mode implicitly fell back");
  Check(
      pa::MakeJerseyCrop(view, {-1000, -1000, 10, 10}, 320, 960, pa::JerseyRoiMode::kBox).reason ==
          pa::JerseyCropReason::kOutsideImage,
      "offscreen crop was scheduled");
  Check(
      pa::MakeJerseyCrop(view, {-50000, -50000, 100000, 100000}, 320, 960, pa::JerseyRoiMode::kBox).reason ==
          pa::JerseyCropReason::kTooLarge,
      "oversized crop escaped hard cap");
  Check(!pa::MakeJerseyCrop(view, {0, 0, 100, 100}, 0, 960, pa::JerseyRoiMode::kBox), "zero metadata width accepted");
  Check(
      pa::JerseyScratchBytes(0) == 0 && pa::JerseyScratchBytes(9) == 0 &&
          pa::JerseyScratchBytes(8) == 8 * pa::JerseyScratchBytes(1),
      "scratch capacity invalid");
}

void CropTests(const fs::path& path) {
  const auto document = YAML::LoadFile((path / "fixtures.json").string());
  auto bgr = cv::imread((path / document["image"].as<std::string>()).string());
  Check(!bgr.empty(), "missing scene");
  cv::Mat rgba;
  cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
  const size_t pitch = (rgba.cols * 4 + 255) / 256 * 256;
  std::vector<uint8_t> pixels(pitch * rgba.rows, 253), packed(pixels.size(), 197);
  for (int y = 0; y < rgba.rows; ++y)
    for (int x = 0; x < rgba.cols; ++x) {
      auto pixel = rgba.at<cv::Vec4b>(y, x);
      uint32_t ten = static_cast<uint32_t>((x + y) % 4) << 30;
      for (int c = 0; c < 3; ++c) {
        pixels[y * pitch + x * 4 + c] = pixel[c];
        ten |= ((static_cast<uint32_t>(pixel[c]) * 1023 + 127) / 255) << (c * 10);
      }
      pixels[y * pitch + x * 4 + 3] = (x * 7 + y) % 256;
      reinterpret_cast<uint32_t*>(packed.data() + y * pitch)[x] = ten;
    }
  std::array<pa::JerseyCrop, 8> crops{};
  std::vector<float> tensor(8 * pa::kJerseyInputElements), ten_tensor(tensor.size());
  Buffer frame(pixels.size()), scratch(pa::JerseyScratchBytes(8)), input(tensor.size() * sizeof(float));
  pa::ImageView image{frame.data, pitch, rgba.cols, rgba.rows, pa::PixelFormat::kRgba8};
  Stream stream;
  float maximum_error = 0;
  size_t pixel_mismatches = 0;
  for (size_t begin = 0; begin < document["crops"].size(); begin += 8) {
    const size_t count = std::min<size_t>(8, document["crops"].size() - begin);
    for (size_t index = 0; index < count; ++index) {
      const auto c = document["crops"][begin + index];
      crops[index] = {c[0].as<int>(), c[1].as<int>(), c[2].as<int>(), c[3].as<int>()};
    }
    Cuda(cudaMemcpyAsync(frame.data, pixels.data(), pixels.size(), cudaMemcpyHostToDevice, stream.stream));
    image.format = pa::PixelFormat::kRgba8;
    Cuda(
        pa::PreprocessJersey(
            image, crops.data(), count, scratch.data, pa::JerseyScratchBytes(8), input.as<float>(), stream.stream));
    Cuda(cudaMemcpyAsync(
        tensor.data(),
        input.data,
        count * pa::kJerseyInputElements * sizeof(float),
        cudaMemcpyDeviceToHost,
        stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    for (size_t index = 0; index < count; ++index) {
      const auto reference =
          Read<float>(path / ("input_" + std::to_string(begin + index) + ".f32"), pa::kJerseyInputElements);
      const auto rgb =
          Read<uint8_t>(path / ("pixels_" + std::to_string(begin + index) + ".rgb"), pa::kJerseyInputElements);
      size_t mismatches = 0;
      for (int c = 0; c < 3; ++c)
        for (int y = 0; y < 32; ++y)
          for (int x = 0; x < 128; ++x) {
            const size_t offset = (c * 32 + y) * 128 + x;
            const float value = tensor[index * pa::kJerseyInputElements + offset];
            maximum_error = std::max(maximum_error, std::abs(value - reference[offset]));
            mismatches += std::lround((value + 1) * 127.5F) != rgb[(y * 128 + x) * 3 + c];
          }
      pixel_mismatches += mismatches;
      std::cout << "PIL crop=" << begin + index << " pixel_mismatches=" << mismatches << '\n';
    }
    Cuda(cudaMemcpyAsync(frame.data, packed.data(), packed.size(), cudaMemcpyHostToDevice, stream.stream));
    image.format = pa::PixelFormat::kRgba10A2;
    Cuda(
        pa::PreprocessJersey(
            image, crops.data(), count, scratch.data, pa::JerseyScratchBytes(8), input.as<float>(), stream.stream));
    Cuda(cudaMemcpyAsync(
        ten_tensor.data(),
        input.data,
        count * pa::kJerseyInputElements * sizeof(float),
        cudaMemcpyDeviceToHost,
        stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    Check(
        std::equal(tensor.begin(), tensor.begin() + count * pa::kJerseyInputElements, ten_tensor.begin()),
        "packed10 RGB/alpha/pitch differs");
  }
  std::cout << "PIL total pixel_mismatches=" << pixel_mismatches << " normalized max_abs=" << maximum_error
            << " scratch8=" << pa::JerseyScratchBytes(8) << '\n';
  Check(pixel_mismatches == 0 && maximum_error < 0.0000002F, "PIL antialiased bicubic parity failed");
  Check(
      pa::PreprocessJersey(image, crops.data(), 1, scratch.data, 0, input.as<float>(), stream.stream) ==
          cudaErrorInvalidValue,
      "undersized scratch accepted");
  crops[0].height = 8193;
  Check(
      pa::PreprocessJersey(
          image, crops.data(), 1, scratch.data, pa::JerseyScratchBytes(8), input.as<float>(), stream.stream) ==
          cudaErrorInvalidValue,
      "oversized crop launched");
}

void DecoderTests(const fs::path& bundle) {
  auto manifest = pa::ParseModelManifest(YAML::LoadFile((bundle / "manifest.json").string()));
  Check(manifest.ok(), manifest.status().ToString());
  auto mapped = pa::MakeJerseyVocabulary(*manifest);
  Check(mapped.ok(), mapped.status().ToString());
  auto vocabulary = *mapped;
  std::swap(vocabulary.digits[1], vocabulary.digits[5]); // Must honor actual map, not assume numeric token order.
  std::array<int, 10> digits{};
  for (int token = 0; token < 95; ++token)
    if (vocabulary.digits[token] >= 0)
      digits[vocabulary.digits[token]] = token;
  const int eos = vocabulary.eos_index;
  std::vector<float> logits(8 * 3 * 95, -20), actions(8 * 60, -5);
  auto winner = [&](int b, int p, int token) { logits[(b * 3 + p) * 95 + token] = 10; };
  winner(0, 0, digits[0]);
  winner(0, 1, digits[0]);
  winner(0, 2, eos);
  winner(1, 0, digits[7]);
  winner(1, 1, eos);
  winner(1, 2, digits[2]);
  winner(2, 0, eos);
  winner(3, 0, 11);
  winner(3, 1, eos); // non-digit beats all digits
  winner(4, 0, digits[1]);
  winner(4, 1, digits[2]);
  winner(4, 2, digits[3]);
  winner(5, 0, digits[2]);
  winner(5, 1, eos);
  logits[5 * 3 * 95 + 94] = std::numeric_limits<float>::quiet_NaN();
  winner(6, 0, digits[3]);
  winner(6, 0, digits[6]);
  winner(6, 1, eos); // first token wins tie
  winner(7, 0, digits[9]);
  winner(7, 1, eos);
  logits[(7 * 3 + 2) * 95] = std::numeric_limits<float>::infinity(); // ignored after EOS
  for (int b = 0; b < 8; ++b)
    actions[b * 60 + (b + 2)] = 8;
  actions[0] = 8; // first argmax among tied classes0/2
  actions[60 + 7] = std::numeric_limits<float>::infinity();
  for (int c = 0; c < 60; ++c)
    actions[2 * 60 + c] = 10000; // stable softmax large, tied
  std::array<pa::JerseyObservation, 8> jerseys{};
  std::array<pa::ActionObservation, 8> predictions{};
  Buffer in(logits.size() * sizeof(float)), out(sizeof(jerseys)), action_in(actions.size() * sizeof(float)),
      action_out(sizeof(predictions));
  Stream stream;
  Cuda(cudaMemcpyAsync(in.data, logits.data(), logits.size() * sizeof(float), cudaMemcpyHostToDevice, stream.stream));
  Cuda(pa::DecodeJersey(in.as<float>(), 8, vocabulary, out.as<pa::JerseyObservation>(), stream.stream));
  Cuda(cudaMemcpyAsync(jerseys.data(), out.data, sizeof(jerseys), cudaMemcpyDeviceToHost, stream.stream));
  Cuda(cudaMemcpyAsync(
      action_in.data, actions.data(), actions.size() * sizeof(float), cudaMemcpyHostToDevice, stream.stream));
  Cuda(pa::ReduceAction(action_in.as<float>(), 8, action_out.as<pa::ActionObservation>(), stream.stream));
  Cuda(
      cudaMemcpyAsync(predictions.data(), action_out.data, sizeof(predictions), cudaMemcpyDeviceToHost, stream.stream));
  Cuda(cudaStreamSynchronize(stream.stream));
  Check(
      std::string(jerseys[0].text) == "00" && jerseys[0].length == 2 && jerseys[0].confidence > .999F,
      "leading zeroes lost");
  Check(std::string(jerseys[1].text) == "7" && jerseys[1].length == 1, "single digit/EOS failed");
  for (size_t index : {2U, 3U, 4U, 5U})
    Check(jerseys[index].length == 0 && jerseys[index].confidence == 0, "invalid jersey accepted");
  const char expected = '0' + vocabulary.digits[std::min(digits[3], digits[6])];
  Check(
      jerseys[6].text[0] == expected && std::abs(jerseys[6].confidence - .5F) < .00001,
      "tie/full-vocabulary confidence failed");
  Check(std::string(jerseys[7].text) == "9", "unused positions after EOS were consumed");
  Check(
      predictions[0].label == 0 && std::abs(predictions[0].confidence - .5F) < .0001, "action tie probability failed");
  Check(predictions[1].label == -1 && predictions[1].confidence == 0, "nonfinite action accepted");
  Check(
      predictions[2].label == 0 && std::abs(predictions[2].confidence - 1.0F / 60) < .0000001,
      "stable full60 softmax failed");
  Check(
      pa::ReduceAction(action_in.as<float>(), 0, action_out.as<pa::ActionObservation>(), stream.stream) ==
          cudaErrorInvalidValue,
      "zero batch launched");
  std::cout << "metadata ROI and compact jersey/action decoder tests passed\n";
}
int main(int argc, char** argv) {
  try {
    Check(argc == 3, "usage: semantic_gpu_test PIL_FIXTURES JERSEY_BUNDLE");
    RoiTests();
    Cuda(cudaSetDevice(0));
    CropTests(argv[1]);
    DecoderTests(argv[2]);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
