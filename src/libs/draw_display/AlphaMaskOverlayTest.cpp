#include "hstream/src/libs/draw_display/AlphaMaskOverlay.h"

#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace d = hm::draw_display;
namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t result) {
  if (result != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(result));
}
double Sample(const std::vector<uint8_t>& mask, int width, int height, size_t pitch, double u, double v) {
  const double x = u * width - .5, y = v * height - .5;
  const int ix = std::floor(x), iy = std::floor(y);
  const double fx = x - ix, fy = y - iy;
  double result = 0;
  for (int j = 0; j < 2; ++j)
    for (int i = 0; i < 2; ++i)
      if (ix + i >= 0 && ix + i < width && iy + j >= 0 && iy + j < height)
        result += mask[(iy + j) * pitch + ix + i] * (i ? fx : 1 - fx) * (j ? fy : 1 - fy);
  return result / 255;
}
void Pixels() {
  constexpr unsigned width = 96, height = 64, left = 7, top = 3;
  constexpr size_t pitch = 128 * 4, mask_pitch = 19;
  std::vector<uint8_t> mask(mask_pitch * 11, 0xa5), original(pitch * (height + 8), 0xa5), actual(original.size());
  for (unsigned y = 0; y < 11; ++y)
    for (unsigned x = 0; x < 12; ++x)
      mask[y * mask_pitch + x] = (x * 71 + y * 19) % 256;
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < width; ++x) {
      const uint32_t pixel = 33U | (66U << 8) | (99U << 16) | (255U << 24);
      std::memcpy(original.data() + (y + top) * pitch + (x + left) * 4, &pixel, 4);
    }
  void *output{}, *texture{};
  cudaStream_t stream{};
  Gpu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  Gpu(cudaMalloc(&output, original.size()));
  Gpu(cudaMalloc(&texture, mask.size()));
  Gpu(cudaMemcpy(texture, mask.data(), mask.size(), cudaMemcpyHostToDevice));
  const d::AlphaMaskView view{static_cast<const uint8_t*>(texture), mask_pitch, 12, 11};
  const d::analytics::ImageView image{static_cast<uint8_t*>(output) + top * pitch + left * 4, pitch, width, height};
  const std::array<d::AlphaMaskMapping, 3> mappings = {{
      {0, 0, 1.0F / width, 0, 0, 1.0F / height},
      {-.3F, .2F, .8F / width, .5F / width, -.4F / height, 1.2F / height},
      {1, 1, -1.0F / width, 0, 0, -1.0F / height},
  }};
  for (const auto& mapping : mappings) {
    Gpu(cudaMemcpyAsync(output, original.data(), original.size(), cudaMemcpyHostToDevice, stream));
    Gpu(d::DrawAlphaMask(image, view, mapping, {0, 1, 0, .28F}, stream));
    Gpu(cudaMemcpyAsync(actual.data(), output, actual.size(), cudaMemcpyDeviceToHost, stream));
    Gpu(cudaStreamSynchronize(stream));
    for (unsigned y = 0; y < height + 8; ++y)
      for (unsigned x = 0; x < pitch; ++x) {
        const size_t index = y * pitch + x;
        if (y < top || y >= top + height || x < left * 4 || x >= (left + width) * 4) {
          Check(actual[index] == original[index], "mask overwrote letterbox or pitch guard");
          continue;
        }
        const double px = (x / 4 - left) + .5, py = (y - top) + .5;
        const double u = mapping.origin_u + px * mapping.step_x_u + py * mapping.step_y_u;
        const double v = mapping.origin_v + px * mapping.step_x_v + py * mapping.step_y_v;
        const double alpha = .28 * Sample(mask, 12, 11, mask_pitch, u, v);
        const int expected =
            x % 4 == 3 ? 255 : std::lround(original[index] * (1 - alpha) + (x % 4 == 1 ? 255 : 0) * alpha);
        Check(std::abs(int(actual[index]) - expected) <= 1, "mask bilinear/inverse mapping differs from reference");
      }
  }
  const auto before = actual;
  auto invalid = mappings[0];
  invalid.step_x_u = NAN;
  Check(
      d::DrawAlphaMask(image, view, invalid, {0, 1, 0, .28F}, stream) == cudaErrorInvalidValue, "NaN mapping accepted");
  auto oversized = view;
  oversized.pitch = 32U * 1024U * 1024U;
  Check(
      d::DrawAlphaMask(image, oversized, mappings[0], {0, 1, 0, .28F}, stream) == cudaErrorInvalidValue,
      "mask allocation budget ignored");
  Gpu(cudaMemcpy(actual.data(), output, actual.size(), cudaMemcpyDeviceToHost));
  Check(actual == before, "invalid mask request modified video");
  std::vector<uint8_t> retained(mask.size());
  Gpu(cudaMemcpy(retained.data(), texture, retained.size(), cudaMemcpyDeviceToHost));
  Check(retained == mask, "immutable mask was modified");
  Gpu(cudaFree(output));
  Gpu(cudaFree(texture));
  Gpu(cudaStreamDestroy(stream));
}
} // namespace
int main() {
  try {
    Gpu(cudaSetDevice(0));
    Pixels();
    std::cout << "alpha_mask linear_border=1 inverse_affine=1 letterbox_guards=1 input_immutable=1 bounded=1\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
