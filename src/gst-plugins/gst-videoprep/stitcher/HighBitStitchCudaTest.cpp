#include "hstream/src/gst-plugins/gst-videoprep/playcropper/ShadowToneCurve.h"
#include "hstream/src/gst-plugins/gst-videoprep/stitcher/cudaHighBitStitch.h"

#include "cupano/pano/cudaPano.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>

#include <opencv2/core.hpp>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

bool cuda_ok(cudaError_t result, const char* operation) {
  if (result != cudaSuccess) {
    std::cerr << operation << " failed: " << cudaGetErrorString(result) << '\n';
    return false;
  }
  return true;
}

uint32_t pack_rgb10a2(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha = 3) {
  return (red & 0x3ffu) | ((green & 0x3ffu) << 10) | ((blue & 0x3ffu) << 20) | ((alpha & 0x3u) << 30);
}

uint8_t composed_grade(float value, float shadow_lift_percent, float exposure) {
  float normalized = value / 255.0f;
  if (shadow_lift_percent > 0.0f && normalized > 0.0f && normalized < hm::playcropper::kShadowLiftVideoStart) {
    normalized = hm::playcropper::evaluate_shadow_lift_curve(normalized, shadow_lift_percent, false);
  }
  return static_cast<uint8_t>(hm::playcropper::clamp_shadow_value(
      normalized * 255.0f * hm::playcropper::exposure_gain(exposure) + 0.5f, 0.0f, 255.0f));
}

bool exposure_match_accepts_half4() {
  cv::Mat left(8, 8, CV_16FC4, cv::Scalar(100.0, 110.0, 120.0, 255.0));
  cv::Mat right(8, 8, CV_16FC4, cv::Scalar(60.0, 70.0, 80.0, 255.0));
  cv::Mat prepared_left = hm::pano::prepare_exposure_match_image(left);
  cv::Mat prepared_right = hm::pano::prepare_exposure_match_image(right);
  if (prepared_left.type() != CV_32FC3 || prepared_right.type() != CV_32FC3) {
    std::cerr << "Exposure preparation did not convert half4 inputs to float3\n";
    return false;
  }
  cv::Mat seam(8, 8, CV_8U, cv::Scalar(1));
  seam.colRange(4, seam.cols).setTo(0);
  const std::optional<cv::Scalar> adjustment =
      hm::pano::match_seam_images(prepared_left, prepared_right, seam, /*N=*/2, cv::Point(0, 0), cv::Point(0, 0));
  if (!adjustment.has_value()) {
    std::cerr << "Exposure matching rejected prepared half4 inputs\n";
    return false;
  }
  for (int channel = 0; channel < 3; ++channel) {
    if (std::abs((*adjustment)[channel] - 20.0) > 0.01) {
      std::cerr << "Exposure matching returned an incorrect half4 adjustment: " << *adjustment << '\n';
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  constexpr int kWidth = 1024;
  constexpr int kHeight = 2;
  constexpr size_t kPackedPitch = kWidth * sizeof(uint32_t);
  std::vector<uint32_t> packed(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      packed[y * kWidth + x] = pack_rgb10a2(x, x, x);
    }
  }

  uint32_t* device_packed = nullptr;
  half4* device_half = nullptr;
  uchar4* device_rgba = nullptr;
  size_t half_pitch = 0;
  size_t rgba_pitch = 0;
  cudaStream_t stream = nullptr;
  bool ok = cuda_ok(
                cudaMalloc(reinterpret_cast<void**>(&device_packed), packed.size() * sizeof(uint32_t)),
                "cudaMalloc(input)") &&
      cuda_ok(cudaMallocPitch(reinterpret_cast<void**>(&device_half), &half_pitch, kWidth * sizeof(half4), kHeight),
              "cudaMallocPitch(half)") &&
      cuda_ok(cudaMallocPitch(reinterpret_cast<void**>(&device_rgba), &rgba_pitch, kWidth * sizeof(uchar4), kHeight),
              "cudaMallocPitch(rgba)") &&
      cuda_ok(cudaStreamCreate(&stream), "cudaStreamCreate") &&
      cuda_ok(cudaMemcpyAsync(
                  device_packed, packed.data(), packed.size() * sizeof(uint32_t), cudaMemcpyHostToDevice, stream),
              "cudaMemcpyAsync(input)");
  if (!ok) {
    if (stream) {
      cudaStreamDestroy(stream);
    }
    cudaFree(device_rgba);
    cudaFree(device_half);
    cudaFree(device_packed);
    return 1;
  }

  NvBufSurfaceParams input_params{};
  input_params.width = kWidth;
  input_params.height = kHeight;
  input_params.pitch = kPackedPitch;
  input_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709;
  input_params.dataPtr = device_packed;
  ok = cuda_ok(
      hm::stitcher::unpackRgb10A2ToHalf4(&input_params, device_half, half_pitch, stream), "unpackRgb10A2ToHalf4");

  std::vector<half4> unpacked(kWidth * kHeight);
  ok = ok &&
      cuda_ok(
           cudaMemcpy2DAsync(
               unpacked.data(),
               kWidth * sizeof(half4),
               device_half,
               half_pitch,
               kWidth * sizeof(half4),
               kHeight,
               cudaMemcpyDeviceToHost,
               stream),
           "cudaMemcpy2DAsync(half)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(half)");
  float previous = -1.0f;
  for (int code = 0; ok && code < kWidth; ++code) {
    const half4& pixel = unpacked[code];
    const float value = __half2float(pixel.x);
    const float expected = static_cast<float>(code) * 255.0f / 1023.0f;
    if (std::abs(value - expected) > 0.126f || value <= previous || __half2float(pixel.y) != value ||
        __half2float(pixel.z) != value || __half2float(pixel.w) != 255.0f) {
      std::cerr << "FP16 unpack did not preserve 10-bit code " << code << " (actual " << value << ", expected "
                << expected << ")\n";
      ok = false;
      break;
    }
    previous = value;
  }

  NvBufSurfaceParams output_params{};
  output_params.width = kWidth;
  output_params.height = kHeight;
  output_params.pitch = rgba_pitch;
  output_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  output_params.dataPtr = device_rgba;
  ok = ok &&
      cuda_ok(
           hm::stitcher::convertHalf4ToRgba8(
               device_half,
               half_pitch,
               kWidth,
               kHeight,
               &output_params,
               /*rotation_degrees=*/0.0,
               /*shadow_lift_percent=*/0.0f,
               /*lift_shadow_black_point=*/false,
               /*exposure=*/0.0f,
               stream),
           "convertHalf4ToRgba8(identity)");
  std::vector<uchar4> rgba(kWidth * kHeight);
  ok = ok &&
      cuda_ok(
           cudaMemcpy2DAsync(
               rgba.data(),
               kWidth * sizeof(uchar4),
               device_rgba,
               rgba_pitch,
               kWidth * sizeof(uchar4),
               kHeight,
               cudaMemcpyDeviceToHost,
               stream),
           "cudaMemcpy2DAsync(rgba)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(rgba)");
  for (int y = 0; ok && y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      const uint8_t expected = static_cast<uint8_t>(__half2float(unpacked[y * kWidth + x].x));
      const uchar4 pixel = rgba[y * kWidth + x];
      if (pixel.x != expected || pixel.y != expected || pixel.z != expected || pixel.w != 255) {
        std::cerr << "Identity conversion changed pixel " << x << "," << y << '\n';
        ok = false;
        break;
      }
    }
  }
  if (ok &&
      (rgba[kWidth - 1].x != 255 || rgba[(kHeight - 1) * kWidth].w != 255 || rgba[kHeight * kWidth - 1].x != 255)) {
    std::cerr << "Identity conversion failed to preserve the last row or column\n";
    ok = false;
  }

  constexpr float kShadowLift = 100.0f;
  constexpr float kExposure = 1.0f;
  ok = ok &&
      cuda_ok(
           hm::stitcher::convertHalf4ToRgba8(
               device_half,
               half_pitch,
               kWidth,
               kHeight,
               &output_params,
               /*rotation_degrees=*/0.0,
               kShadowLift,
               /*lift_shadow_black_point=*/false,
               kExposure,
               stream),
           "convertHalf4ToRgba8(grade)") &&
      cuda_ok(
           cudaMemcpy2DAsync(
               rgba.data(),
               kWidth * sizeof(uchar4),
               device_rgba,
               rgba_pitch,
               kWidth * sizeof(uchar4),
               kHeight,
               cudaMemcpyDeviceToHost,
               stream),
           "cudaMemcpy2DAsync(graded rgba)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(graded rgba)");
  for (const int code : {0, 64, 256, 614, 1023}) {
    const float source = __half2float(unpacked[code].x);
    const uint8_t expected = code == 0 ? 0 : composed_grade(source, kShadowLift, kExposure);
    const uchar4 pixel = rgba[code];
    if (pixel.x != expected || pixel.y != expected || pixel.z != expected || pixel.w != 255) {
      std::cerr << "Composed FP16 grading mismatch at 10-bit code " << code << '\n';
      ok = false;
    }
  }

  ok = ok &&
      cuda_ok(
           hm::stitcher::convertHalf4ToCalibrationRgba8(
               device_half,
               half_pitch,
               kWidth,
               kHeight,
               &output_params,
               /*rotation_degrees=*/0.0,
               stream),
           "convertHalf4ToCalibrationRgba8") &&
      cuda_ok(
           cudaMemcpy2DAsync(
               rgba.data(),
               kWidth * sizeof(uchar4),
               device_rgba,
               rgba_pitch,
               kWidth * sizeof(uchar4),
               kHeight,
               cudaMemcpyDeviceToHost,
               stream),
           "cudaMemcpy2DAsync(calibration rgba)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(calibration rgba)");
  for (const int code : {0, 64, 256, 614, 1023}) {
    const uint8_t expected = static_cast<uint8_t>(__half2float(unpacked[code].x));
    const uchar4 pixel = rgba[code];
    if (pixel.x != expected || pixel.y != expected || pixel.z != expected || pixel.w != 255) {
      std::cerr << "Calibration conversion was contaminated by tone controls at 10-bit code " << code << '\n';
      ok = false;
    }
  }

  ok = ok && exposure_match_accepts_half4();

  NvBufSurfaceParams invalid_params = input_params;
  invalid_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  if (hm::stitcher::unpackRgb10A2ToHalf4(&invalid_params, device_half, half_pitch, stream) != cudaErrorInvalidValue ||
      !hm::stitcher::isRgb10A2ColorFormat(NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_2020) ||
      hm::stitcher::isRgb10A2ColorFormat(NVBUF_COLOR_FORMAT_RGBA)) {
    std::cerr << "RGB10A2 format validation failed\n";
    ok = false;
  }

  cudaStreamDestroy(stream);
  cudaFree(device_rgba);
  cudaFree(device_half);
  cudaFree(device_packed);
  return ok ? 0 : 1;
}
