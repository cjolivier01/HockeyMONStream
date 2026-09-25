#include "hstream/src/libs/player_analytics/runtime/NativeEngine.h"
#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

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
  Check(file.good(), "missing fixture: " + path.string());
  std::vector<float> values(count);
  file.read(reinterpret_cast<char*>(values.data()), count * sizeof(float));
  Check(
      file.gcount() == static_cast<std::streamsize>(count * sizeof(float)) && file.peek() == EOF, "wrong fixture size");
  return values;
}
float Difference(const std::vector<float>& reference, const std::vector<float>& actual) {
  Check(reference.size() == actual.size(), "different tensor sizes");
  float maximum = 0;
  for (size_t i = 0; i < actual.size(); ++i) {
    Check(std::isfinite(actual[i]), "nonfinite native value");
    maximum = std::max(maximum, std::abs(reference[i] - actual[i]));
  }
  return maximum;
}

int main(int argc, char** argv) {
  try {
    Check(argc == 3, "usage: pose_fixture_test POSE_BUNDLE OFFLINE_FIXTURE_DIRECTORY");
    const fs::path fixture = argv[2];
    const auto config = YAML::LoadFile((fixture / "fixture.json").string());
    auto bgr = cv::imread((fixture / config["image"].as<std::string>()).string());
    Check(!bgr.empty(), "missing offline scene");
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    const auto box = config["roi_xywh"];
    const pa::PoseRoi roi{
        {box[0].as<float>(), box[1].as<float>(), box[2].as<float>(), box[3].as<float>()},
        config["metadata_size"][0].as<float>(),
        config["metadata_size"][1].as<float>()};
    auto reference_input = Read(fixture / "input.f32", pa::kPoseInputElements);
    auto reference_pose = Read(fixture / "reference_pose.f32", 17 * 3);
    std::vector<float> actual_input(pa::kPoseInputElements);
    std::array<std::vector<float>, 2> reference, actual;
    for (size_t index = 0; index < 2; ++index) {
      const size_t count = 17 * (index == 0 ? 384 : 512);
      reference[index] = Read(fixture / ("reference_" + std::to_string(index) + ".f32"), count);
      actual[index].resize(count);
    }
    pa::Pose pose{};
    Cuda(cudaSetDevice(0));
    auto loaded = pa::NativeEngine::Load(argv[1], pa::ModelFeature::kPose, 0, 1);
    Check(loaded.ok(), loaded.status().ToString());
    auto engine = std::move(*loaded);
    Buffer frame(rgba.step * rgba.rows), affines(sizeof(pa::PoseAffine)), results(sizeof(pa::Pose));
    pa::ImageView view{frame.data, rgba.step, rgba.cols, rgba.rows, pa::PixelFormat::kRgba8};
    const auto affine = pa::MakePoseAffine(view, roi);
    Check(affine.ok(), affine.status().ToString());
    Stream stream; // All host/device buffers survive both success and failure fences.
    Cuda(cudaMemcpyAsync(frame.data, rgba.data, rgba.step * rgba.rows, cudaMemcpyHostToDevice, stream.stream));
    Cuda(cudaMemcpyAsync(affines.data, &*affine, sizeof(*affine), cudaMemcpyHostToDevice, stream.stream));
    Cuda(pa::PreprocessPose(view, affines.as<pa::PoseAffine>(), 1, engine->input(), stream.stream));
    const auto status = engine->Enqueue(1, stream.stream);
    Check(status.ok(), status.ToString());
    Cuda(
        pa::DecodePose(
            engine->output(0),
            engine->output(1),
            affines.as<pa::PoseAffine>(),
            1,
            results.as<pa::Pose>(),
            stream.stream));
    // Validation-only readback of pixels/distributions; production copies only compact Pose.
    Cuda(cudaMemcpyAsync(
        actual_input.data(),
        engine->input(),
        actual_input.size() * sizeof(float),
        cudaMemcpyDeviceToHost,
        stream.stream));
    for (size_t index = 0; index < 2; ++index)
      Cuda(cudaMemcpyAsync(
          actual[index].data(),
          engine->output(index),
          actual[index].size() * sizeof(float),
          cudaMemcpyDeviceToHost,
          stream.stream));
    Cuda(cudaMemcpyAsync(&pose, results.data, sizeof(pose), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    const float preprocessing = Difference(reference_input, actual_input);
    std::cout << "real player MMPose preprocessing max_abs=" << preprocessing << '\n';
    Check(preprocessing < 0.00001F, "GPU crop differs from MMPose/OpenCV reference");
    // Use the prepared precision contract, then independently constrain the
    // decoded joints below. Raw FP16 SimCC error alone is not pixel error.
    const bool fp16 = engine->manifest().precision == "fp16";
    const float absolute_tolerance = fp16 ? 0.08F : 0.0001F;
    const float relative_tolerance = fp16 ? 0.03F : 0.0005F;
    bool distributions_match = true;
    for (size_t index = 0; index < 2; ++index) {
      const float error = Difference(reference[index], actual[index]);
      std::cout << "real player SimCC " << index << " PyTorch max_abs=" << error << '\n';
      for (size_t i = 0; i < actual[index].size(); ++i)
        distributions_match &= std::abs(actual[index][i] - reference[index][i]) <=
            absolute_tolerance + relative_tolerance * std::abs(reference[index][i]);
    }
    double maximum_position = 0, maximum_score = 0;
    size_t exact_argmax = 0;
    for (size_t j = 0; j < 17; ++j) {
      // Direct decoder parity isolates CUDA reduction from FP16 model variation.
      const auto xb = actual[0].begin() + j * 384, yb = actual[1].begin() + j * 512;
      const auto xi = std::max_element(xb, xb + 384) - xb, yi = std::max_element(yb, yb + 512) - yb;
      const double x = (affine->x0 + affine->dx * xi * 0.5) * affine->surface_to_metadata_x;
      const double y = (affine->y0 + affine->dy * yi * 0.5) * affine->surface_to_metadata_y;
      const float score = std::clamp(std::min(xb[xi], yb[yi]), 0.0F, 1.0F);
      Check(
          std::abs(pose[j].x - x) < 0.0002 && std::abs(pose[j].y - y) < 0.0002 && pose[j].confidence == score,
          "GPU compact decoder differs from direct TensorRT output reduction");
      maximum_position = std::max(
          maximum_position,
          std::hypot(
              static_cast<double>(pose[j].x - reference_pose[j * 3]),
              static_cast<double>(pose[j].y - reference_pose[j * 3 + 1])));
      maximum_score =
          std::max(maximum_score, static_cast<double>(std::abs(pose[j].confidence - reference_pose[j * 3 + 2])));
      const auto rx = reference[0].begin() + j * 384, ry = reference[1].begin() + j * 512;
      exact_argmax += (xi == std::max_element(rx, rx + 384) - rx && yi == std::max_element(ry, ry + 512) - ry);
    }
    const double input_pixel =
        std::max(affine->dx * affine->surface_to_metadata_x, affine->dy * affine->surface_to_metadata_y);
    std::cout << "real player 17-joint argmax agreement=" << exact_argmax
              << "/17 max_metadata_position=" << maximum_position
              << " max_model_position=" << maximum_position / input_pixel << " max_confidence=" << maximum_score
              << '\n';
    Check(distributions_match, "real player inference exceeds prepared precision tolerances");
    Check(maximum_position / input_pixel <= 1.0 && maximum_score < 0.01, "compact pose diverges from upstream decoder");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
