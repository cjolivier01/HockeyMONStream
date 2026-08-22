#include "hstream/src/gst-plugins/gst-videoprep/playcropper/ShadowToneCurve.h"
#include "hstream/src/gst-plugins/gst-videoprep/playcropper/cudaPlayCropper.h"

#include <cuda_runtime.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

namespace {

bool cuda_ok(cudaError_t result, const char* operation) {
  if (result != cudaSuccess) {
    std::cerr << operation << " failed: " << cudaGetErrorString(result) << '\n';
    return false;
  }
  return true;
}

bool transform_sample(
    const std::array<uint8_t, 16>& input,
    float sample_x,
    float sample_y,
    float lift_percent,
    std::array<uint8_t, 4>* output,
    bool lift_black_point = false,
    float exposure = 0.0f) {
  if (!output) {
    return false;
  }

  uint8_t* device_input = nullptr;
  uint8_t* device_output = nullptr;
  cudaStream_t stream = nullptr;
  if (!cuda_ok(cudaMalloc(&device_input, input.size()), "cudaMalloc(input)") ||
      !cuda_ok(cudaMalloc(&device_output, output->size()), "cudaMalloc(output)") ||
      !cuda_ok(cudaStreamCreate(&stream), "cudaStreamCreate") ||
      !cuda_ok(
          cudaMemcpyAsync(device_input, input.data(), input.size(), cudaMemcpyHostToDevice, stream),
          "cudaMemcpyAsync(input)")) {
    if (stream) {
      cudaStreamDestroy(stream);
    }
    cudaFree(device_output);
    cudaFree(device_input);
    return false;
  }

  NvBufSurfaceParams input_params{};
  input_params.width = 2;
  input_params.height = 2;
  input_params.pitch = 8;
  input_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  input_params.dataPtr = device_input;
  NvBufSurfaceParams output_params{};
  output_params.width = 1;
  output_params.height = 1;
  output_params.pitch = 4;
  output_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  output_params.dataPtr = device_output;

  const bool ok = cuda_ok(
                      hm::playcropper::combinedTransform(
                          &input_params,
                          hm::BBox(0, 0, 2, 2),
                          0.0f,
                          hm::Point{.x = 0.0f, .y = 0.0f},
                          hm::BBox(sample_x, sample_y, sample_x + 1.0f, sample_y + 1.0f),
                          &output_params,
                          hm::BBox(0, 0, 1, 1),
                          lift_percent,
                          lift_black_point,
                          exposure,
                          stream),
                      "combinedTransform") &&
      cuda_ok(cudaMemcpyAsync(output->data(), device_output, output->size(), cudaMemcpyDeviceToHost, stream),
              "cudaMemcpyAsync(output)") &&
      cuda_ok(cudaStreamSynchronize(stream), "cudaStreamSynchronize");

  cudaStreamDestroy(stream);
  cudaFree(device_output);
  cudaFree(device_input);
  return ok;
}

bool transform_pixel(
    const std::array<uint8_t, 4>& pixel,
    float lift_percent,
    std::array<uint8_t, 4>* output,
    bool lift_black_point = false,
    float exposure = 0.0f) {
  std::array<uint8_t, 16> input{};
  for (size_t offset = 0; offset < input.size(); offset += pixel.size()) {
    std::copy(pixel.begin(), pixel.end(), input.begin() + offset);
  }
  return transform_sample(input, 0.0f, 0.0f, lift_percent, output, lift_black_point, exposure);
}

uint8_t exposed_channel(uint8_t value, float setting) {
  const float lifted = hm::playcropper::evaluate_exposure(value / 255.0f, setting) * 255.0f + 0.5f;
  return static_cast<uint8_t>(hm::playcropper::clamp_shadow_value(lifted, 0.0f, 255.0f));
}

std::array<uint8_t, 4> graded_pixel(
    const std::array<uint8_t, 4>& pixel,
    float shadow_lift_percent,
    bool lift_black_point = false,
    float exposure = 0.0f) {
  float red = pixel[0] / 255.0f;
  float green = pixel[1] / 255.0f;
  float blue = pixel[2] / 255.0f;
  if (shadow_lift_percent > 0.0f) {
    hm::playcropper::evaluate_shadow_lift_rgb(
        &red,
        &green,
        &blue,
        hm::playcropper::shadow_lift_gamma(shadow_lift_percent),
        hm::playcropper::shadow_lift_amount(shadow_lift_percent),
        lift_black_point);
  }
  const float exposure_gain = hm::playcropper::exposure_gain(exposure);
  const auto quantize = [exposure_gain](float value) {
    return static_cast<uint8_t>(
        hm::playcropper::clamp_shadow_value(value * 255.0f * exposure_gain + 0.5f, 0.0f, 255.0f));
  };
  return {quantize(red), quantize(green), quantize(blue), pixel[3]};
}

} // namespace

int main() {
  const std::array<uint8_t, 4> input = {25, 50, 100, 17};
  std::array<uint8_t, 4> output{};
  if (!transform_pixel(input, 0.0f, &output) || output != input) {
    std::cerr << "Zero shadow lift must preserve the existing fused-kernel pixels exactly\n";
    return 1;
  }
  if (!transform_pixel(input, 100.0f, &output)) {
    return 1;
  }
  const std::array<uint8_t, 4> expected = graded_pixel(input, 100.0f);
  if (output != expected || output[0] <= input[0] || output[1] <= input[1] || output[2] <= input[2]) {
    std::cerr << "CUDA shadow lift must match the fitted luma gamma while preserving alpha\n";
    return 1;
  }

  const std::array<uint8_t, 4> black_input = {0, 0, 0, 203};
  if (!transform_pixel(black_input, 100.0f, &output) || output != black_input) {
    std::cerr << "CUDA shadow lift must preserve exact black and alpha\n";
    return 1;
  }
  const std::array<uint8_t, 4> white_input = {255, 255, 255, 203};
  if (!transform_pixel(white_input, 100.0f, &output) || output != white_input) {
    std::cerr << "CUDA shadow lift must preserve exact white and alpha\n";
    return 1;
  }
  const std::array<uint8_t, 4> midtone_input = {153, 153, 153, 203};
  if (!transform_pixel(midtone_input, 100.0f, &output) || output != graded_pixel(midtone_input, 100.0f) ||
      output[0] <= midtone_input[0]) {
    std::cerr << "CUDA shadow lift must lift midtones instead of stopping at 60% signal\n";
    return 1;
  }

  const std::array<uint8_t, 4> exposure_input = {0, 64, 128, 241};
  const std::array<uint8_t, 4> exposure_expected = {
      0,
      exposed_channel(exposure_input[1], 1.3f),
      exposed_channel(exposure_input[2], 1.3f),
      exposure_input[3],
  };
  if (!transform_pixel(exposure_input, 0.0f, &output, false, 1.3f) || output != exposure_expected || output[0] != 0 ||
      output[2] != 201) {
    std::cerr << "CUDA exposure must match the measured gain while preserving exact black and alpha\n";
    return 1;
  }

  const std::array<uint8_t, 4> composed_input = {16, 64, 128, 203};
  const std::array<uint8_t, 4> composed_expected = graded_pixel(composed_input, 100.0f, false, 1.0f);
  if (!transform_pixel(composed_input, 100.0f, &output, false, 1.0f) || output != composed_expected) {
    std::cerr << "CUDA exposure must compose after the shadow and midtone lift\n";
    return 1;
  }

  const std::array<uint8_t, 4> black_point_input = {0, 0, 0, 203};
  const std::array<uint8_t, 4> black_point_expected = graded_pixel(black_point_input, 100.0f, true);
  if (!transform_pixel(black_point_input, 100.0f, &output, true) || output != black_point_expected || output[0] == 0 ||
      output[0] != output[1] || output[1] != output[2]) {
    std::cerr << "CUDA black-point lift must raise exact black neutrally while preserving alpha\n";
    return 1;
  }

  const std::array<uint8_t, 16> protected_bilinear_input = {
      200,
      200,
      200,
      17,
      201,
      201,
      201,
      17,
      201,
      201,
      201,
      17,
      201,
      201,
      201,
      17,
  };
  const std::array<uint8_t, 4> bilinear_identity = {200, 200, 200, 17};
  if (!transform_sample(protected_bilinear_input, 0.5f, 0.5f, 0.0f, &output) || output != bilinear_identity) {
    std::cerr << "Disabled shadow lift must preserve legacy interpolation and quantization\n";
    return 1;
  }
  if (!transform_sample(protected_bilinear_input, 0.5f, 0.5f, 100.0f, &output) || output[0] <= bilinear_identity[0] ||
      output[0] != output[1] || output[1] != output[2] || output[3] != bilinear_identity[3]) {
    std::cerr << "Enabled shadow lift must reach upper midtones without modifying alpha\n";
    return 1;
  }
  return 0;
}
