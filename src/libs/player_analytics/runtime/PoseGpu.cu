#include "hstream/src/libs/player_analytics/runtime/PoseGpu.h"

#include <cuda_runtime.h>
#include <math_constants.h>

#include <cstdint>

namespace hm::player_analytics {
namespace {

static_assert(sizeof(Pose) == kCocoJoints * sizeof(Keypoint));

__device__ int ReadChannel(const ImageView image, int x, int y, int channel) {
  if (x < 0 || y < 0 || x >= image.width || y >= image.height)
    return 0;
  const auto* row = static_cast<const uint8_t*>(image.data) + static_cast<size_t>(y) * image.pitch;
  if (image.format == PixelFormat::kRgba8)
    return row[x * 4 + channel];
  const uint32_t packed = reinterpret_cast<const uint32_t*>(row)[x];
  const uint32_t value = (packed >> (channel * 10)) & 1023U;
  return (value * 255U + 511U) / 1023U;
}

__global__ void PreprocessKernel(ImageView image, const PoseAffine* affines, float* output) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= kPoseInputWidth || y >= kPoseInputHeight)
    return;
  const PoseAffine affine = affines[blockIdx.z];
  // OpenCV's warpAffine uses separate 10-bit fixed point delta/translation,
  // then rounds to its five-bit interpolation table. Its y translation and
  // row delta are rounded together; x delta is rounded separately. Signed shifts floor.
  const long long fixed_x = (__double2ll_rn(affine.x0 * 1024.0) + __double2ll_rn(affine.dx * x * 1024.0) + 16) >> 5;
  const long long fixed_y = (__double2ll_rn((affine.y0 + affine.dy * y) * 1024.0) + 16) >> 5;
  const int left = static_cast<int>(fixed_x >> 5);
  const int top = static_cast<int>(fixed_y >> 5);
  const int fx = static_cast<int>(fixed_x & 31);
  const int fy = static_cast<int>(fixed_y & 31);
  constexpr float mean[3] = {123.675F, 116.28F, 103.53F};
  constexpr float standard_deviation[3] = {58.395F, 57.12F, 57.375F};
  for (int channel = 0; channel < 3; ++channel) {
    const int weighted = ReadChannel(image, left, top, channel) * (32 - fx) * (32 - fy) +
        ReadChannel(image, left + 1, top, channel) * fx * (32 - fy) +
        ReadChannel(image, left, top + 1, channel) * (32 - fx) * fy +
        ReadChannel(image, left + 1, top + 1, channel) * fx * fy;
    const float pixel = static_cast<float>((weighted + 512) >> 10);
    const size_t index =
        (static_cast<size_t>(blockIdx.z) * 3 + channel) * kPoseInputWidth * kPoseInputHeight + y * kPoseInputWidth + x;
    output[index] = (pixel - mean[channel]) / standard_deviation[channel];
  }
}

struct Maximum {
  float value;
  int index;
  int invalid;
};

__device__ Maximum Merge(Maximum a, Maximum b) {
  if (b.value > a.value || (b.value == a.value && b.index < a.index)) {
    b.invalid |= a.invalid;
    return b;
  }
  a.invalid |= b.invalid;
  return a;
}

__global__ void DecodeKernel(const float* simcc_x, const float* simcc_y, const PoseAffine* affines, Keypoint* results) {
  constexpr int kThreads = 128;
  __shared__ Maximum x_maximum[kThreads];
  __shared__ Maximum y_maximum[kThreads];
  const int keypoint = blockIdx.y;
  const int batch = blockIdx.x;
  const int tid = threadIdx.x;
  const size_t joint = static_cast<size_t>(batch) * kCocoJoints + keypoint;
  Maximum mx{-CUDART_INF_F, 0, 0};
  Maximum my{-CUDART_INF_F, 0, 0};
  for (int i = tid; i < 384; i += kThreads) {
    const float value = simcc_x[joint * 384 + i];
    mx = Merge(mx, {isfinite(value) ? value : -CUDART_INF_F, i, !isfinite(value)});
  }
  for (int i = tid; i < 512; i += kThreads) {
    const float value = simcc_y[joint * 512 + i];
    my = Merge(my, {isfinite(value) ? value : -CUDART_INF_F, i, !isfinite(value)});
  }
  x_maximum[tid] = mx;
  y_maximum[tid] = my;
  __syncthreads();
  for (int offset = kThreads / 2; offset; offset /= 2) {
    if (tid < offset) {
      x_maximum[tid] = Merge(x_maximum[tid], x_maximum[tid + offset]);
      y_maximum[tid] = Merge(y_maximum[tid], y_maximum[tid + offset]);
    }
    __syncthreads();
  }
  if (!tid) {
    const auto x = x_maximum[0];
    const auto y = y_maximum[0];
    const float confidence = fminf(x.value, y.value);
    Keypoint result{};
    if (!x.invalid && !y.invalid && confidence > 0) {
      const auto affine = affines[batch];
      result.x = static_cast<float>((affine.x0 + affine.dx * (x.index * 0.5)) * affine.surface_to_metadata_x);
      result.y = static_cast<float>((affine.y0 + affine.dy * (y.index * 0.5)) * affine.surface_to_metadata_y);
      result.confidence = fminf(confidence, 1.0F);
      if (!isfinite(result.x) || !isfinite(result.y))
        result = {};
    }
    results[joint] = result;
  }
}

} // namespace

cudaError_t PreprocessPose(
    const ImageView& image,
    const PoseAffine* affines,
    size_t batch,
    float* input,
    cudaStream_t stream) {
  if (!image.data || !affines || !input || !stream || batch == 0 || batch > kMaximumBatch || image.width < 1 ||
      image.height < 1 || image.width > 65536 || image.height > 65536 ||
      image.pitch < static_cast<size_t>(image.width) * 4 || image.pitch % 4 != 0 ||
      image.pitch > SIZE_MAX / static_cast<size_t>(image.height) ||
      reinterpret_cast<uintptr_t>(image.data) % alignof(uint32_t) != 0 ||
      (image.format != PixelFormat::kRgba8 && image.format != PixelFormat::kRgba10A2))
    return cudaErrorInvalidValue;
  const dim3 block(16, 16);
  const dim3 grid((kPoseInputWidth + 15) / 16, (kPoseInputHeight + 15) / 16, batch);
  PreprocessKernel<<<grid, block, 0, stream>>>(image, affines, input);
  return cudaGetLastError();
}

cudaError_t DecodePose(
    const float* x,
    const float* y,
    const PoseAffine* affines,
    size_t batch,
    Pose* results,
    cudaStream_t stream) {
  if (!x || !y || !affines || !results || !stream || batch == 0 || batch > kMaximumBatch)
    return cudaErrorInvalidValue;
  DecodeKernel<<<dim3(batch, kCocoJoints), 128, 0, stream>>>(x, y, affines, reinterpret_cast<Keypoint*>(results));
  return cudaGetLastError();
}

} // namespace hm::player_analytics
