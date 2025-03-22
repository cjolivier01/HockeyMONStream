#include <cuda_runtime.h>
#include <math.h>
#include "ScoreboardKernels.h"

// --- Helper: bilinear interpolation for uchar3.
// Converts pixel values to float for interpolation, then rounds to uchar.
__device__ inline uchar3 bilinearInterpolate(const uchar3* src, int width, int height, float x, float y) {
  int ix = floorf(x);
  int iy = floorf(y);
  float a = x - ix;
  float b = y - iy;

  if (ix < 0 || iy < 0 || ix + 1 >= width || iy + 1 >= height)
    return make_uchar3(0, 0, 0);

  uchar3 p00 = src[iy * width + ix];
  uchar3 p10 = src[iy * width + (ix + 1)];
  uchar3 p01 = src[(iy + 1) * width + ix];
  uchar3 p11 = src[(iy + 1) * width + (ix + 1)];

  uchar3 result;
  result.x =
      (unsigned char)((1 - a) * (1 - b) * p00.x + a * (1 - b) * p10.x + (1 - a) * b * p01.x + a * b * p11.x + 0.5f);
  result.y =
      (unsigned char)((1 - a) * (1 - b) * p00.y + a * (1 - b) * p10.y + (1 - a) * b * p01.y + a * b * p11.y + 0.5f);
  result.z =
      (unsigned char)((1 - a) * (1 - b) * p00.z + a * (1 - b) * p10.z + (1 - a) * b * p01.z + a * b * p11.z + 0.5f);
  return result;
}

// --- Kernel: Crop a rectangular ROI.
__global__ void cropKernel(
    const uchar3* input,
    int inW,
    int inH,
    uchar3* output,
    int outW,
    int outH,
    int offX,
    int offY) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x < outW && y < outH) {
    int inX = x + offX;
    int inY = y + offY;
    if (inX < inW && inY < inH)
      output[y * outW + x] = input[inY * inW + inX];
  }
}

// --- Kernel: Bilinear resize.
__global__ void resizeKernel(const uchar3* input, int inW, int inH, uchar3* output, int outW, int outH) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x < outW && y < outH) {
    float gx = (float)x * (inW - 1) / (outW - 1);
    float gy = (float)y * (inH - 1) / (outH - 1);
    uchar3 res = bilinearInterpolate(input, inW, inH, gx, gy);
    output[y * outW + x] = res;
  }
}

// --- Kernel: Warp perspective with bilinear interpolation.
// The transform is provided as three float3 values (m0, m1, m2),
// which are rows of a 3x3 matrix in row-major order.
__global__ void warpPerspectiveKernel(
    const uchar3* input,
    int inW,
    int inH,
    uchar3* output,
    int outW,
    int outH,
    float3 m0,
    float3 m1,
    float3 m2) {
  int x = blockIdx.x * blockDim.x + threadIdx.x;
  int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= outW || y >= outH)
    return;
  // Compute warped coordinate as in OpenCV’s device calcCoord:
  float denom = m2.x * x + m2.y * y + m2.z;
  float u = (m0.x * x + m0.y * y + m0.z) / denom;
  float v = (m1.x * x + m1.y * y + m1.z) / denom;
  uchar3 value;
  if (u < 0.f || v < 0.f || u >= (inW - 1) || v >= (inH - 1))
    value = make_uchar3(0, 0, 0);
  else
    value = bilinearInterpolate(input, inW, inH, u, v);
  output[y * outW + x] = value;
}

extern "C" {

void launchCropKernel(
    const uchar3* d_input,
    int inW,
    int inH,
    uchar3* d_output,
    int outW,
    int outH,
    int offX,
    int offY) {
  dim3 block(16, 16);
  dim3 grid((outW + block.x - 1) / block.x, (outH + block.y - 1) / block.y);
  cropKernel<<<grid, block>>>(d_input, inW, inH, d_output, outW, outH, offX, offY);
  cudaDeviceSynchronize();
}

void launchResizeKernel(const uchar3* d_input, int inW, int inH, uchar3* d_output, int outW, int outH) {
  dim3 block(16, 16);
  dim3 grid((outW + block.x - 1) / block.x, (outH + block.y - 1) / block.y);
  resizeKernel<<<grid, block>>>(d_input, inW, inH, d_output, outW, outH);
  cudaDeviceSynchronize();
}

void launchWarpPerspectiveKernel(
    const uchar3* d_input,
    int inW,
    int inH,
    uchar3* d_output,
    int outW,
    int outH,
    float3 m0,
    float3 m1,
    float3 m2) {
  dim3 block(16, 16);
  dim3 grid((outW + block.x - 1) / block.x, (outH + block.y - 1) / block.y);
  warpPerspectiveKernel<<<grid, block>>>(d_input, inW, inH, d_output, outW, outH, m0, m1, m2);
  cudaDeviceSynchronize();
}

} // extern "C"
