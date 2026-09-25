#include "hstream/src/gst-plugins/gst-videoprep/playcropper/ShadowToneCurve.h"
#include "hstream/src/gst-plugins/gst-videoprep/stitcher/cudaHighBitStitch.h"

#include "cupano/cuda/cudaRemap.h"
#include "cupano/pano/cudaMat.h"

#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <cstring>

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

uint32_t pack_bgr10a2(uint32_t red, uint32_t green, uint32_t blue, uint32_t alpha = 3) {
  return (blue & 0x3ffu) | ((green & 0x3ffu) << 10) | ((red & 0x3ffu) << 20) | ((alpha & 0x3u) << 30);
}

uint8_t composed_grade(float value, float shadow_lift_percent, float exposure) {
  float normalized = value / 255.0f;
  if (shadow_lift_percent > 0.0f) {
    normalized = hm::playcropper::evaluate_shadow_lift_luma(normalized, shadow_lift_percent, false);
  }
  return static_cast<uint8_t>(hm::playcropper::clamp_shadow_value(
      normalized * 255.0f * hm::playcropper::exposure_gain(exposure) + 0.5f, 0.0f, 255.0f));
}

uint32_t composed_grade_10(float value, float shadow_lift_percent, float exposure) {
  float normalized = value / 255.0f;
  if (shadow_lift_percent > 0.0f) {
    normalized = hm::playcropper::evaluate_shadow_lift_luma(normalized, shadow_lift_percent, false);
  }
  const float graded =
      hm::playcropper::clamp_shadow_value(normalized * 255.0f * hm::playcropper::exposure_gain(exposure), 0.0f, 255.0f);
  return static_cast<uint32_t>(std::lrint(graded * (1023.0f / 255.0f)));
}

} // namespace

int main() {
  constexpr int kWidth = 1024;
  constexpr int kHeight = 2;
  constexpr size_t kPackedPitch = kWidth * sizeof(uint32_t);
  std::vector<uint32_t> packed(kWidth * kHeight);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      packed[y * kWidth + x] = pack_rgb10a2(x, x, x, (x + y) % 4);
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
  // Compare the fused conversion against the actual legacy CUDA unpack kernel.
  // Identity remapping covers all 1024 codes and all four packed alpha values.
  if (ok) {
    std::vector<uint16_t> mx(kWidth * kHeight), my(kWidth * kHeight);
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) {
        mx[y * kWidth + x] = x;
        my[y * kWidth + x] = y;
      }
    hm::CudaMat<uint16_t> map_x(1, kWidth, kHeight), map_y(1, kWidth, kHeight);
    hm::CudaMat<half4> remapped(1, kWidth, kHeight);
    ok = map_x.is_valid() && map_y.is_valid() && remapped.is_valid() &&
        cuda_ok(
             cudaMemcpyAsync(map_x.data(), mx.data(), mx.size() * sizeof(uint16_t), cudaMemcpyHostToDevice, stream),
             "map x") &&
        cuda_ok(
             cudaMemcpyAsync(map_y.data(), my.data(), my.size() * sizeof(uint16_t), cudaMemcpyHostToDevice, stream),
             "map y") &&
        cuda_ok(
             batched_remap_kernel_ex_offset(
                 CudaSurface<Rgb10A2>{reinterpret_cast<Rgb10A2*>(device_packed), kWidth, kHeight, kPackedPitch},
                 remapped.surface(),
                 map_x.data(),
                 map_y.data(),
                 Rgb10A2{},
                 1,
                 kWidth,
                 kHeight,
                 0,
                 0,
                 false,
                 stream),
             "fused RGB10 remap");
    std::vector<half4> fused(unpacked.size());
    ok = ok &&
        cuda_ok(
             cudaMemcpyAsync(fused.data(), remapped.data(), remapped.size(), cudaMemcpyDeviceToHost, stream),
             "fused download") &&
        cuda_ok(cudaStreamSynchronize(stream), "fused synchronize");
    if (ok && std::memcmp(fused.data(), unpacked.data(), fused.size() * sizeof(half4)) != 0) {
      std::cerr << "Fused RGB10 remap differs from legacy unpack\n";
      ok = false;
    }
  }
  float previous = -1.0f;
  for (int code = 0; ok && code < kWidth; ++code) {
    const half4& pixel = unpacked[code];
    const float value = __half2float(pixel.x);
    const float expected = static_cast<float>(code) * 255.0f / 1023.0f;
    if (std::abs(value - expected) > 0.126f || value <= previous || __half2float(pixel.y) != value ||
        __half2float(pixel.z) != value || __half2float(pixel.w) != 255.0f) {
      std::cerr << "FP16 unpack did not preserve 10-bit code " << code << " with full stitch-valid alpha (actual "
                << value << ", expected " << expected << ", alpha " << __half2float(pixel.w) << ")\n";
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

  output_params.colorFormat = NVBUF_COLOR_FORMAT_BGRA_10_10_10_2_709;
  ok = ok &&
      cuda_ok(
           hm::stitcher::convertHalf4ToBgr10A2(
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
           "convertHalf4ToBgr10A2(identity)");
  std::vector<uint32_t> bgr10(kWidth * kHeight);
  ok = ok &&
      cuda_ok(
           cudaMemcpy2DAsync(
               bgr10.data(),
               kWidth * sizeof(uint32_t),
               device_rgba,
               rgba_pitch,
               kWidth * sizeof(uint32_t),
               kHeight,
               cudaMemcpyDeviceToHost,
               stream),
           "cudaMemcpy2DAsync(bgr10)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(bgr10)");
  for (const int code : {0, 1, 64, 256, 614, 1023}) {
    if (bgr10[code] != pack_rgb10a2(code, code, code)) {
      std::cerr << "BGR10A2 identity conversion changed 10-bit code " << code << " to 0x" << std::hex << bgr10[code]
                << std::dec << '\n';
      ok = false;
    }
  }

  half4 colored{};
  colored.x = __float2half(255.0f);
  colored.y = __float2half(127.5f);
  colored.z = __float2half(0.25f);
  colored.w = __float2half(255.0f);
  ok = ok &&
      cuda_ok(
           cudaMemcpyAsync(device_half, &colored, sizeof(colored), cudaMemcpyHostToDevice, stream),
           "cudaMemcpyAsync(colored half)") &&
      cuda_ok(
           hm::stitcher::convertHalf4ToBgr10A2(
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
           "convertHalf4ToBgr10A2(colored)") &&
      cuda_ok(
           cudaMemcpyAsync(bgr10.data(), device_rgba, sizeof(uint32_t), cudaMemcpyDeviceToHost, stream),
           "cudaMemcpyAsync(colored bgr10)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(colored bgr10)");
  if (ok) {
    const uint32_t actual_blue = bgr10[0] & 0x3ffu;
    const uint32_t actual_green = (bgr10[0] >> 10) & 0x3ffu;
    const uint32_t actual_red = (bgr10[0] >> 20) & 0x3ffu;
    const uint32_t actual_alpha = (bgr10[0] >> 30) & 0x3u;
    if (actual_red != 1023 || actual_green < 510 || actual_green > 512 || actual_blue != 1 || actual_alpha != 3) {
      std::cerr << "BGR10A2 conversion stored colored channels in the wrong fields (actual 0x" << std::hex << bgr10[0]
                << std::dec << ")\n";
      ok = false;
    }
  }
  ok = ok &&
      cuda_ok(
           cudaMemcpyAsync(device_half, unpacked.data(), sizeof(half4), cudaMemcpyHostToDevice, stream),
           "cudaMemcpyAsync(restore half)");

  constexpr float kShadowLift = 100.0f;
  constexpr float kExposure = 1.0f;
  ok = ok &&
      cuda_ok(
           hm::stitcher::convertHalf4ToBgr10A2(
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
           "convertHalf4ToBgr10A2(grade)") &&
      cuda_ok(
           cudaMemcpy2DAsync(
               bgr10.data(),
               kWidth * sizeof(uint32_t),
               device_rgba,
               rgba_pitch,
               kWidth * sizeof(uint32_t),
               kHeight,
               cudaMemcpyDeviceToHost,
               stream),
           "cudaMemcpy2DAsync(graded bgr10)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize(graded bgr10)");
  const int graded_code = 256;
  const uint32_t expected_graded_code =
      composed_grade_10(__half2float(unpacked[graded_code].x), kShadowLift, kExposure);
  if (ok &&
      (expected_graded_code <= static_cast<uint32_t>(graded_code) ||
       bgr10[graded_code] != pack_bgr10a2(expected_graded_code, expected_graded_code, expected_graded_code))) {
    std::cerr << "BGR10A2 output did not apply the FP16 shadow/exposure grade before quantization\n";
    ok = false;
  }
  output_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;

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
