#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace pa = hm::player_analytics;

void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Cuda(cudaError_t result) {
  if (result != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(result));
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

cv::Mat Reference(const cv::Mat& image, const pa::PoseRoi& roi) {
  const auto& b = roi.metadata_box;
  const float sx = image.cols / roi.metadata_width, sy = image.rows / roi.metadata_height;
  cv::Point2f center((b.left + b.width / 2) * sx, (b.top + b.height / 2) * sy);
  float width = b.width * sx * 1.25F, height = b.height * sy * 1.25F;
  width = std::max(width, height * 0.75F);
  const cv::Point2f left(center.x - width / 2, center.y);
  const cv::Point2f direction = center - left;
  const std::array<cv::Point2f, 3> source{{center, left, left + cv::Point2f(-direction.y, direction.x)}};
  const std::array<cv::Point2f, 3> target{{{96, 128}, {0, 128}, {0, 224}}};
  cv::Mat affine;
  cv::getAffineTransform(source.data(), target.data()).convertTo(affine, CV_32F);
  cv::Mat output;
  cv::warpAffine(image, output, affine, {192, 256}, cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar());
  return output;
}

int main() {
  try {
    Cuda(cudaSetDevice(0));
    constexpr int width = 319, height = 233;
    constexpr size_t pitch = 1344;
    std::vector<uint8_t> rgba(pitch * height, 173);
    cv::Mat image(height, width, CV_8UC3);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        for (int c = 0; c < 3; ++c) {
          const uint8_t pixel = (x * (c + 2) + y * (5 - c) + (x % 11) * 9) % 256;
          rgba[y * pitch + x * 4 + c] = pixel;
          image.at<cv::Vec3b>(y, x)[c] = pixel;
        }
    const std::array<pa::PoseRoi, 8> rois{{
        {{25.125F, 10.75F, 42.375F, 80.125F}, 159.5F, 466},
        {{-10.25F, -9.375F, 65.5F, 210.25F}, 159.5F, 466},
        {{130.5F, 395.25F, 60.5F, 95.5F}, 159.5F, 466},
        {{-400, -400, 20, 20}, 159.5F, 466},
        {{10.31237F, 91.65437F, 11.97651F, 78.23456F}, 319, 233},
        {{72.62143F, 15.98763F, 102.54567F, 1.38543F}, 319, 233},
        {{121.67523F, 204.75281F, 10.45367F, 28.97651F}, 319, 233},
        {{151.74839F, 95.41253F, 8.32347F, 17.23561F}, 319, 233},
    }};
    Buffer frame(rgba.size());
    Buffer affines(sizeof(pa::PoseAffine) * rois.size());
    Buffer tensor(sizeof(float) * pa::kPoseInputElements * rois.size());
    Buffer simcc_x(sizeof(float) * rois.size() * 17 * 384);
    Buffer simcc_y(sizeof(float) * rois.size() * 17 * 512);
    Buffer results(sizeof(pa::Pose) * rois.size());
    Stream stream; // Destroy before every GPU allocation, including exceptions.
    pa::ImageView view{frame.data, pitch, width, height, pa::PixelFormat::kRgba8};
    std::array<pa::PoseAffine, rois.size()> transforms{};
    for (size_t i = 0; i < rois.size(); ++i) {
      auto transform = pa::MakePoseAffine(view, rois[i]);
      Check(transform.ok(), "valid fractional/padded ROI rejected");
      transforms[i] = *transform;
    }
    Cuda(cudaMemcpyAsync(frame.data, rgba.data(), rgba.size(), cudaMemcpyHostToDevice, stream.stream));
    Cuda(cudaMemcpyAsync(affines.data, transforms.data(), sizeof(transforms), cudaMemcpyHostToDevice, stream.stream));
    Cuda(pa::PreprocessPose(view, affines.as<pa::PoseAffine>(), rois.size(), tensor.as<float>(), stream.stream));
    std::vector<float> actual(pa::kPoseInputElements * rois.size());
    Cuda(cudaMemcpyAsync(
        actual.data(), tensor.data, actual.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    constexpr float mean[3] = {123.675F, 116.28F, 103.53F}, deviation[3] = {58.395F, 57.12F, 57.375F};
    float maximum_error = 0;
    for (size_t b = 0; b < rois.size(); ++b) {
      const auto reference = Reference(image, rois[b]);
      for (int c = 0; c < 3; ++c)
        for (int y = 0; y < 256; ++y)
          for (int x = 0; x < 192; ++x) {
            const float expected = (reference.at<cv::Vec3b>(y, x)[c] - mean[c]) / deviation[c];
            const auto index = (b * 3 + c) * 256 * 192 + y * 192 + x;
            maximum_error = std::max(maximum_error, std::abs(expected - actual[index]));
          }
    }
    std::cout << "OpenCV fractional/out-of-bounds/nonuniform scale max error=" << maximum_error << '\n';
    Check(maximum_error < 0.00001F, "CUDA crop differs from independent OpenCV affine/uint8 reference");

    // Same RGB pixels encoded as 10:10:10:2 must produce exactly the same model
    // tensor; alpha differs deliberately and pitch includes unread padding.
    std::vector<uint8_t> rgb10(rgba.size(), 249);
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x) {
        uint32_t packed = static_cast<uint32_t>((x + y) % 4) << 30;
        for (int c = 0; c < 3; ++c)
          packed |= ((static_cast<uint32_t>(rgba[y * pitch + x * 4 + c]) * 1023 + 127) / 255) << (c * 10);
        reinterpret_cast<uint32_t*>(rgb10.data() + y * pitch)[x] = packed;
      }
    Cuda(cudaMemcpyAsync(frame.data, rgb10.data(), rgb10.size(), cudaMemcpyHostToDevice, stream.stream));
    view.format = pa::PixelFormat::kRgba10A2;
    Cuda(pa::PreprocessPose(view, affines.as<pa::PoseAffine>(), rois.size(), tensor.as<float>(), stream.stream));
    std::vector<float> packed_actual(actual.size());
    Cuda(cudaMemcpyAsync(
        packed_actual.data(), tensor.data, actual.size() * sizeof(float), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    Check(actual == packed_actual, "packed RGB10 channels, pitch or alpha were misinterpreted");

    std::vector<float> xs(rois.size() * 17 * 384, -2), ys(rois.size() * 17 * 512, -3);
    for (size_t j = 0; j < rois.size() * 17; ++j) {
      xs[j * 384 + 101] = xs[j * 384 + 102] = 0.8F; // first argmax wins a tie
      ys[j * 512 + 203] = 0.6F;
    }
    xs[0] = std::numeric_limits<float>::quiet_NaN();
    Cuda(cudaMemcpyAsync(simcc_x.data, xs.data(), xs.size() * sizeof(float), cudaMemcpyHostToDevice, stream.stream));
    Cuda(cudaMemcpyAsync(simcc_y.data, ys.data(), ys.size() * sizeof(float), cudaMemcpyHostToDevice, stream.stream));
    Cuda(
        pa::DecodePose(
            simcc_x.as<float>(),
            simcc_y.as<float>(),
            affines.as<pa::PoseAffine>(),
            rois.size(),
            results.as<pa::Pose>(),
            stream.stream));
    std::array<pa::Pose, rois.size()> poses{};
    Cuda(cudaMemcpyAsync(poses.data(), results.data, sizeof(poses), cudaMemcpyDeviceToHost, stream.stream));
    Cuda(cudaStreamSynchronize(stream.stream));
    Check(poses[0][0].confidence == 0, "nonfinite SimCC joint was not suppressed");
    for (size_t b = 0; b < rois.size(); ++b) {
      const auto& a = transforms[b];
      Check(
          std::abs(poses[b][1].x - (a.x0 + a.dx * 50.5) * a.surface_to_metadata_x) < 0.0001 &&
              std::abs(poses[b][1].y - (a.y0 + a.dy * 101.5) * a.surface_to_metadata_y) < 0.0001 &&
              poses[b][1].confidence == 0.6F,
          "SimCC argmax/inverse metadata transform failed");
    }
    Check(
        pa::PreprocessPose(view, affines.as<pa::PoseAffine>(), 0, tensor.as<float>(), stream.stream) ==
            cudaErrorInvalidValue,
        "zero batch launched work");
    auto invalid = rois[0];
    invalid.metadata_width = 0;
    Check(!pa::MakePoseAffine(view, invalid).ok(), "invalid coordinate dimensions were accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
