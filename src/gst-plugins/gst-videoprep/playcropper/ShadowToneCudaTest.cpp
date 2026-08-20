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
    bool lift_black_point = false) {
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
    bool lift_black_point = false) {
  std::array<uint8_t, 16> input{};
  for (size_t offset = 0; offset < input.size(); offset += pixel.size()) {
    std::copy(pixel.begin(), pixel.end(), input.begin() + offset);
  }
  return transform_sample(input, 0.0f, 0.0f, lift_percent, output, lift_black_point);
}

uint8_t lifted_channel(uint8_t value, float lift_percent, bool lift_black_point = false) {
  const float lifted =
      hm::playcropper::evaluate_shadow_lift_curve(value / 255.0f, lift_percent, lift_black_point) * 255.0f + 0.5f;
  return static_cast<uint8_t>(hm::playcropper::clamp_shadow_value(lifted, 0.0f, 255.0f));
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
  const std::array<uint8_t, 4> expected = {
      lifted_channel(input[0], 100.0f),
      lifted_channel(input[1], 100.0f),
      lifted_channel(input[2], 100.0f),
      input[3],
  };
  if (output != expected || output[0] <= input[0] || output[1] <= input[1] || output[2] <= input[2]) {
    std::cerr << "CUDA shadow lift must match the reference curve while preserving alpha\n";
    return 1;
  }

  const std::array<uint8_t, 4> protected_input = {0, 153, 255, 203};
  if (!transform_pixel(protected_input, 100.0f, &output) || output != protected_input) {
    std::cerr << "CUDA shadow lift must preserve black, the shadow boundary, white, and alpha\n";
    return 1;
  }

  const std::array<uint8_t, 4> black_point_input = {0, 16, 153, 203};
  const std::array<uint8_t, 4> black_point_expected = {
      lifted_channel(black_point_input[0], 100.0f, true),
      lifted_channel(black_point_input[1], 100.0f, true),
      black_point_input[2],
      black_point_input[3],
  };
  if (!transform_pixel(black_point_input, 100.0f, &output, true) || output != black_point_expected || output[0] == 0 ||
      output[1] <= lifted_channel(black_point_input[1], 100.0f, false)) {
    std::cerr << "CUDA black-point lift must visibly raise the toe while protecting the boundary and alpha\n";
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
  const std::array<uint8_t, 4> protected_bilinear_expected = {200, 200, 200, 17};
  if (!transform_sample(protected_bilinear_input, 0.5f, 0.5f, 0.0f, &output) || output != protected_bilinear_expected ||
      !transform_sample(protected_bilinear_input, 0.5f, 0.5f, 100.0f, &output) ||
      output != protected_bilinear_expected) {
    std::cerr << "Shadow lift must not change legacy interpolation or quantization above the shadow boundary\n";
    return 1;
  }
  return 0;
}
