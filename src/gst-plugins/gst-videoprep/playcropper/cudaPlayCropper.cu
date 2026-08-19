#include "hstream/src/gst-plugins/gst-videoprep/playcropper/ShadowToneCurve.h"
#include "hstream/src/gst-plugins/gst-videoprep/playcropper/cudaPlayCropper.h"

#include <cstdint>

namespace hm {
namespace playcropper {
namespace {
// Combined single-kernel approach for crop, rotate, and resize operations
template <typename T>
__global__ void cropRotateResizeKernel(
    const uint8_t* input,
    int input_pitch,
    int input_width,
    int input_height,
    uint8_t* output,
    int output_pitch,
    int output_width,
    int output_height,
    float src_left,
    float src_top,
    float angle,
    float anchor_x,
    float anchor_y,
    float box_left,
    float box_top,
    float box_width,
    float box_height,
    int num_channels,
    float shadow_lift_percent) {
  // Calculate output pixel coordinates
  const int x_out = blockIdx.x * blockDim.x + threadIdx.x;
  const int y_out = blockIdx.y * blockDim.y + threadIdx.y;

  if (x_out >= output_width || y_out >= output_height)
    return;

  // Step 1: Map output coordinates to final crop region
  float norm_x = static_cast<float>(x_out) / output_width;
  float norm_y = static_cast<float>(y_out) / output_height;

  float box_x = box_left + norm_x * box_width;
  float box_y = box_top + norm_y * box_height;

  // Step 2: Inverse rotation to find corresponding pre-rotated coordinates
  float radians = -angle * (M_PI / 180.0f); // Negative for inverse rotation
  float sin_theta = sinf(radians);
  float cos_theta = cosf(radians);

  float dx = box_x - anchor_x;
  float dy = box_y - anchor_y;

  float rotated_x = anchor_x + (dx * cos_theta - dy * sin_theta);
  float rotated_y = anchor_y + (dx * sin_theta + dy * cos_theta);

  // Step 3: Map back to input image space
  float input_x = src_left + rotated_x;
  float input_y = src_top + rotated_y;

  // Check bounds and interpolate
  if (input_x >= 0 && input_x < input_width - 1 && input_y >= 0 && input_y < input_height - 1) {
    // Bilinear interpolation
    int x0 = floorf(input_x);
    int y0 = floorf(input_y);
    int x1 = x0 + 1;
    int y1 = y0 + 1;

    float dx_frac = input_x - x0;
    float dy_frac = input_y - y0;

    // Process each channel
    for (int c = 0; c < num_channels; c++) {
      // Get four nearest pixel values for this channel
      uint8_t p00 = input[y0 * input_pitch + x0 * num_channels + c];
      uint8_t p01 = input[y0 * input_pitch + x1 * num_channels + c];
      uint8_t p10 = input[y1 * input_pitch + x0 * num_channels + c];
      uint8_t p11 = input[y1 * input_pitch + x1 * num_channels + c];

      // Bilinear interpolation
      float p0 = p00 * (1.0f - dx_frac) + p01 * dx_frac;
      float p1 = p10 * (1.0f - dx_frac) + p11 * dx_frac;
      float result = p0 * (1.0f - dy_frac) + p1 * dy_frac;
      const bool is_alpha = num_channels == 4 && c == 3;
      if (!is_alpha && shadow_lift_percent > 0.0f) {
        result = evaluate_shadow_lift_curve(result / 255.0f, shadow_lift_percent) * 255.0f + 0.5f;
      }

      // Write to output
      output[y_out * output_pitch + x_out * num_channels + c] =
          static_cast<uint8_t>(clamp_shadow_value(result, 0.0f, 255.0f));
    }
  }
}
} // namespace

// Function to launch the kernel with appropriate parameters
cudaError_t combinedTransform(
    const NvBufSurfaceParams* in_params,
    const hm::BBox& src_rect,
    float angle,
    const hm::Point& anchor_point,
    const hm::BBox& crop_box,
    NvBufSurfaceParams* out_params,
    const hm::BBox& output_rect,
    float shadow_lift_percent,
    cudaStream_t stream) {
  // Determine number of channels based on color format
  int num_channels = 0;
  switch (in_params->colorFormat) {
    case NVBUF_COLOR_FORMAT_RGBA:
      num_channels = 4;
      break;
    case NVBUF_COLOR_FORMAT_RGB:
      num_channels = 3;
      break;
    case NVBUF_COLOR_FORMAT_GRAY8:
      num_channels = 1;
      break;
    case NVBUF_COLOR_FORMAT_NV12:
      // Special handling for YUV formats would be needed
      return cudaError_t::cudaErrorInvalidTexture;
    default:
      return cudaError_t::cudaErrorInvalidTexture;
  }

  // Get input and output dimensions
  int input_width = in_params->width;
  int input_height = in_params->height;
  int output_width = output_rect.width();
  int output_height = output_rect.height();

  // Set up kernel launch parameters
  dim3 block(16, 16);
  dim3 grid((output_width + block.x - 1) / block.x, (output_height + block.y - 1) / block.y);

  // Launch kernel
  cropRotateResizeKernel<uint8_t><<<grid, block, 0, stream>>>(
      static_cast<uint8_t*>(in_params->dataPtr),
      in_params->pitch,
      input_width,
      input_height,
      static_cast<uint8_t*>(out_params->dataPtr),
      out_params->pitch,
      output_width,
      output_height,
      src_rect.left,
      src_rect.top,
      angle,
      anchor_point.x,
      anchor_point.y,
      crop_box.left,
      crop_box.top,
      crop_box.width(),
      crop_box.height(),
      num_channels,
      shadow_lift_percent);

  // Check for errors
  return cudaGetLastError();
}
} // namespace playcropper
} // namespace hm
