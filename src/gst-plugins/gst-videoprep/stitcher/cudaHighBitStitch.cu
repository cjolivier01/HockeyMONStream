#include "hstream/src/gst-plugins/gst-videoprep/stitcher/cudaHighBitStitch.h"

#include "hstream/src/gst-plugins/gst-videoprep/playcropper/ShadowToneCurve.h"

#include <cmath>
#include <cstdint>

namespace hm::stitcher {
namespace {

constexpr uint32_t kRgb10Mask = 0x3ffu;
constexpr float kRgb10ToVideo = 255.0f / 1023.0f;

__global__ void unpackRgb10A2Kernel(
    const uint8_t* input,
    size_t input_pitch,
    int width,
    int height,
    uint8_t* output,
    size_t output_pitch) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= width || y >= height) {
    return;
  }

  const auto* input_row = reinterpret_cast<const uint32_t*>(input + static_cast<size_t>(y) * input_pitch);
  auto* output_row = reinterpret_cast<half4*>(output + static_cast<size_t>(y) * output_pitch);
  const uint32_t packed = input_row[x];
  output_row[x] = half4{
      __float2half(static_cast<float>(packed & kRgb10Mask) * kRgb10ToVideo),
      __float2half(static_cast<float>((packed >> 10) & kRgb10Mask) * kRgb10ToVideo),
      __float2half(static_cast<float>((packed >> 20) & kRgb10Mask) * kRgb10ToVideo),
      __float2half(static_cast<float>((packed >> 30) & 0x3u) * 85.0f),
  };
}

__device__ float sampleChannel(const half4& pixel, int channel) {
  switch (channel) {
    case 0:
      return __half2float(pixel.x);
    case 1:
      return __half2float(pixel.y);
    case 2:
      return __half2float(pixel.z);
    default:
      return __half2float(pixel.w);
  }
}

__global__ void convertHalf4ToRgba8Kernel(
    const uint8_t* input,
    size_t input_pitch,
    int input_width,
    int input_height,
    uint8_t* output,
    size_t output_pitch,
    int output_width,
    int output_height,
    float cos_angle,
    float sin_angle,
    float center_x,
    float center_y,
    float shadow_lift_gamma_value,
    float shadow_lift_amount_value,
    bool lift_shadow_black_point,
    float exposure_gain_value) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= output_width || y >= output_height) {
    return;
  }

  const float source_x = cos_angle * x + sin_angle * y + (1.0f - cos_angle) * center_x - sin_angle * center_y;
  const float source_y = -sin_angle * x + cos_angle * y + sin_angle * center_x + (1.0f - cos_angle) * center_y;
  auto* output_row = reinterpret_cast<uchar4*>(output + static_cast<size_t>(y) * output_pitch);
  if (source_x < 0.0f || source_y < 0.0f || source_x > input_width - 1 || source_y > input_height - 1) {
    output_row[x] = make_uchar4(0, 0, 0, 0);
    return;
  }

  const int x0 = static_cast<int>(floorf(source_x));
  const int y0 = static_cast<int>(floorf(source_y));
  const int x1 = min(x0 + 1, input_width - 1);
  const int y1 = min(y0 + 1, input_height - 1);
  const float dx = source_x - x0;
  const float dy = source_y - y0;
  const auto* row0 = reinterpret_cast<const half4*>(input + static_cast<size_t>(y0) * input_pitch);
  const auto* row1 = reinterpret_cast<const half4*>(input + static_cast<size_t>(y1) * input_pitch);
  const half4 p00 = row0[x0];
  const half4 p01 = row0[x1];
  const half4 p10 = row1[x0];
  const half4 p11 = row1[x1];

  float interpolated[4] = {};
#pragma unroll
  for (int channel = 0; channel < 4; ++channel) {
    const float p0 = sampleChannel(p00, channel) * (1.0f - dx) + sampleChannel(p01, channel) * dx;
    const float p1 = sampleChannel(p10, channel) * (1.0f - dx) + sampleChannel(p11, channel) * dx;
    interpolated[channel] = p0 * (1.0f - dy) + p1 * dy;
  }

  const bool apply_shadow_lift = shadow_lift_amount_value > 0.0f;
  if (apply_shadow_lift) {
    float red = interpolated[0] / 255.0f;
    float green = interpolated[1] / 255.0f;
    float blue = interpolated[2] / 255.0f;
    playcropper::evaluate_shadow_lift_rgb(
        &red, &green, &blue, shadow_lift_gamma_value, shadow_lift_amount_value, lift_shadow_black_point);
    interpolated[0] = red * 255.0f;
    interpolated[1] = green * 255.0f;
    interpolated[2] = blue * 255.0f;
  }

  uchar4 result{};
  uint8_t* channels = reinterpret_cast<uint8_t*>(&result);
#pragma unroll
  for (int channel = 0; channel < 4; ++channel) {
    float value = interpolated[channel];
    if (channel < 3 && exposure_gain_value > 1.0f) {
      value *= exposure_gain_value;
    }
    if (channel < 3 && (apply_shadow_lift || exposure_gain_value > 1.0f)) {
      value += 0.5f;
    }
    channels[channel] = static_cast<uint8_t>(playcropper::clamp_shadow_value(value, 0.0f, 255.0f));
  }
  output_row[x] = result;
}

} // namespace

bool isRgb10A2ColorFormat(NvBufSurfaceColorFormat format) {
  return format == NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709 || format == NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_2020;
}

cudaError_t unpackRgb10A2ToHalf4(
    const NvBufSurfaceParams* input,
    half4* output,
    size_t output_pitch,
    cudaStream_t stream) {
  if (!input || !input->dataPtr || !output || !isRgb10A2ColorFormat(input->colorFormat)) {
    return cudaErrorInvalidValue;
  }
  const dim3 block(16, 16);
  const dim3 grid((input->width + block.x - 1) / block.x, (input->height + block.y - 1) / block.y);
  unpackRgb10A2Kernel<<<grid, block, 0, stream>>>(
      static_cast<const uint8_t*>(input->dataPtr),
      input->pitch,
      input->width,
      input->height,
      reinterpret_cast<uint8_t*>(output),
      output_pitch);
  return cudaGetLastError();
}

cudaError_t convertHalf4ToRgba8(
    const half4* input,
    size_t input_pitch,
    int input_width,
    int input_height,
    NvBufSurfaceParams* output,
    double rotation_degrees,
    float shadow_lift_percent,
    bool lift_shadow_black_point,
    float exposure,
    cudaStream_t stream) {
  if (!input || !output || !output->dataPtr || output->colorFormat != NVBUF_COLOR_FORMAT_RGBA || input_width < 2 ||
      input_height < 2 || output->width < 1 || output->height < 1) {
    return cudaErrorInvalidValue;
  }
  const float radians = static_cast<float>(-rotation_degrees * M_PI / 180.0);
  const float cos_angle = cosf(radians);
  const float sin_angle = sinf(radians);
  const float center_x = (input_width - 1) * 0.5f;
  const float center_y = (input_height - 1) * 0.5f;
  const dim3 block(16, 16);
  const dim3 grid((output->width + block.x - 1) / block.x, (output->height + block.y - 1) / block.y);
  convertHalf4ToRgba8Kernel<<<grid, block, 0, stream>>>(
      reinterpret_cast<const uint8_t*>(input),
      input_pitch,
      input_width,
      input_height,
      static_cast<uint8_t*>(output->dataPtr),
      output->pitch,
      output->width,
      output->height,
      cos_angle,
      sin_angle,
      center_x,
      center_y,
      playcropper::shadow_lift_gamma(shadow_lift_percent),
      playcropper::shadow_lift_amount(shadow_lift_percent),
      lift_shadow_black_point,
      playcropper::exposure_gain(exposure));
  return cudaGetLastError();
}

cudaError_t convertHalf4ToCalibrationRgba8(
    const half4* input,
    size_t input_pitch,
    int input_width,
    int input_height,
    NvBufSurfaceParams* output,
    double rotation_degrees,
    cudaStream_t stream) {
  return convertHalf4ToRgba8(
      input,
      input_pitch,
      input_width,
      input_height,
      output,
      rotation_degrees,
      /*shadow_lift_percent=*/0.0f,
      /*lift_shadow_black_point=*/false,
      /*exposure=*/0.0f,
      stream);
}

} // namespace hm::stitcher
