#include "hstream/src/libs/highlights/TextureBlend.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
void Check(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t error) {
  Check(error == cudaSuccess, cudaGetErrorString(error));
}
struct Buffer {
  void* data{};
  explicit Buffer(size_t bytes) {
    Gpu(cudaMalloc(&data, bytes));
  }
  ~Buffer() {
    cudaFree(data);
  }
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
};
struct Stream {
  cudaStream_t value{};
  Stream() {
    Gpu(cudaStreamCreateWithFlags(&value, cudaStreamNonBlocking));
  }
  ~Stream() {
    cudaStreamSynchronize(value);
    cudaStreamDestroy(value);
  }
};

void Test(bool ten) {
  using namespace hm::highlights;
  constexpr int width = 19, height = 9, stride = 24;
  constexpr uint32_t guard = 0xa5a5a5a5;
  const uint32_t black = ten ? 0xc0000000u : 0xff000000u;
  const uint32_t red = black | (ten ? 1023 : 255);
  std::vector<uint32_t> pixels(stride * height + 2, guard);
  Buffer image(pixels.size() * sizeof(uint32_t)), texture(4 * sizeof(uint32_t));
  Stream stream;
  auto* data = static_cast<uint32_t*>(image.data) + 1;
  Gpu(cudaMemcpyAsync(image.data, pixels.data(), pixels.size() * 4, cudaMemcpyHostToDevice, stream.value));
  Gpu(ClearFrame(data, stride * 4, width, height, ten, stream.value));
  // One opaque red texel, enlarged and clipped on the left. A transparent
  // green texture subsequently covers it; premultiplied interpolation must
  // leave the existing image intact.
  const std::array<uint32_t, 4> texels{{0xff0000ffu, 0x0000ff00u, 0, 0}};
  Gpu(cudaMemcpyAsync(texture.data, texels.data(), sizeof(texels), cudaMemcpyHostToDevice, stream.value));
  Gpu(BlendTexture(data, stride * 4, width, height, ten, texture.data, 1, 1, -2, 1, 7, 4, stream.value));
  Gpu(BlendTexture(
      data,
      stride * 4,
      width,
      height,
      ten,
      static_cast<uint32_t*>(texture.data) + 1,
      1,
      1,
      0,
      0,
      width,
      height,
      stream.value));
  Gpu(cudaMemcpyAsync(pixels.data(), image.data, pixels.size() * 4, cudaMemcpyDeviceToHost, stream.value));
  Gpu(cudaStreamSynchronize(stream.value));
  Check(pixels.front() == guard && pixels.back() == guard, "frame guards overwritten");
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < stride; ++x) {
      const auto expected = x >= width ? guard : (x < 5 && y >= 1 && y < 5 ? red : black);
      Check(pixels[1 + y * stride + x] == expected, "clipping, alpha, or pitch mismatch");
    }
  // Halfway between opaque red and transparent green must remain red with
  // half coverage, with no green fringe in either video precision.
  const std::array<uint32_t, 2> edge{{0xff0000ffu, 0x0000ff00u}};
  Gpu(cudaMemcpyAsync(texture.data, edge.data(), sizeof(edge), cudaMemcpyHostToDevice, stream.value));
  Gpu(BlendTexture(data, stride * 4, width, height, ten, texture.data, 2, 1, 10, 0, 3, 1, stream.value));
  Gpu(cudaMemcpyAsync(pixels.data(), image.data, pixels.size() * 4, cudaMemcpyDeviceToHost, stream.value));
  Gpu(cudaStreamSynchronize(stream.value));
  Check(pixels[12] == (black | (ten ? 512 : 128)), "premultiplied edge interpolation mismatch");
  Check(ClearFrame(nullptr, stride * 4, width, height, ten, stream.value) == cudaErrorInvalidValue, "null clear");
  Check(ClearFrame(data, 4, width, height, ten, stream.value) == cudaErrorInvalidValue, "short pitch");
  Check(ClearFrame(data, stride * 4, 0, height, ten, stream.value) == cudaErrorInvalidValue, "zero width");
  Check(
      BlendTexture(
          data,
          stride * 4,
          width,
          height,
          ten,
          texture.data,
          1,
          1,
          std::numeric_limits<int>::max(),
          0,
          2,
          1,
          stream.value) == cudaErrorInvalidValue,
      "coordinate overflow");
}
} // namespace

int main() {
  try {
    cudaDeviceProp device{};
    Gpu(cudaGetDeviceProperties(&device, 0));
    std::cout << "GPU: " << device.name << '\n';
    Test(false);
    Test(true);
    std::cout << "Highlight blend: pitch, guards, clipping, alpha, interpolation and invalid inputs passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
