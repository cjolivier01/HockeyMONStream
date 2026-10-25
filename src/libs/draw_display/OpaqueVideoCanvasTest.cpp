#include "hstream/src/libs/draw_display/OpaqueVideoCanvas.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace d = hm::draw_display;
namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Gpu(cudaError_t result) {
  if (result != cudaSuccess)
    throw std::runtime_error(cudaGetErrorString(result));
}
void Pixels() {
  constexpr unsigned width = 37, height = 19;
  constexpr size_t pitch = 48 * 4;
  std::vector<uint8_t> original(pitch * height, 0xa5), actual(original.size());
  const d::VideoRectangle video{5, 3, 29, 13};
  for (unsigned y = video.top; y < video.top + video.height; ++y)
    for (unsigned x = video.left; x < video.left + video.width; ++x) {
      auto* pixel = original.data() + y * pitch + x * 4;
      pixel[0] = x;
      pixel[1] = y;
      pixel[2] = 17;
      pixel[3] = x % 3 == 0 ? 0 : x % 3 == 1 ? 128 : 255;
    }
  void* device{};
  cudaStream_t stream{};
  Gpu(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking));
  Gpu(cudaMalloc(&device, original.size()));
  Gpu(cudaMemcpyAsync(device, original.data(), original.size(), cudaMemcpyHostToDevice, stream));
  const d::analytics::ImageView image{device, pitch, width, height};
  Gpu(d::FinishOpaqueVideoCanvas(image, video, stream));
  Gpu(cudaMemcpyAsync(actual.data(), device, actual.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  for (unsigned y = 0; y < height; ++y)
    for (unsigned x = 0; x < pitch; ++x) {
      const auto offset = y * pitch + x;
      const bool inside =
          x / 4 >= video.left && x / 4 < video.left + video.width && y >= video.top && y < video.top + video.height;
      const unsigned expected = x >= width * 4 ? original[offset] : x % 4 == 3 ? 255 : inside ? original[offset] : 0;
      Check(actual[offset] == expected, "Opaque canvas changed video RGB or left dirty bars/pitch guards");
    }
  const auto before = actual;
  Check(
      d::FinishOpaqueVideoCanvas(image, {UINT32_MAX, 0, 1, 1}, stream) == cudaErrorInvalidValue,
      "Overflowing rectangle was admitted");
  Check(
      d::FinishOpaqueVideoCanvas(image, {0, 0, 0, 1}, stream) == cudaErrorInvalidValue, "Empty rectangle was admitted");
  Gpu(cudaMemcpy(actual.data(), device, actual.size(), cudaMemcpyDeviceToHost));
  Check(actual == before, "Invalid canvas geometry modified pixels");

  // Qt's RGB video pass ignores source alpha. A 40% red overlay over a
  // transparent panorama pixel must blend with video RGB, not become full red.
  d::analytics::CommandList commands;
  Check(commands.AddFill(6, 3, 7, 4, {1, 0, 0, .4F}), "Could not create blend fixture");
  d::analytics::Compositor compositor;
  Check(compositor.Render(image, commands, stream).status == d::analytics::RenderStatus::kOk, "Blend failed");
  Gpu(cudaMemcpyAsync(actual.data(), device, actual.size(), cudaMemcpyDeviceToHost, stream));
  Gpu(cudaStreamSynchronize(stream));
  const auto offset = 3 * pitch + 6 * 4;
  Check(
      actual[offset] >= 105 && actual[offset] <= 106 && actual[offset + 3] == 255,
      "Transparent video corner changed Qt overlay opacity");
  Gpu(cudaFree(device));
  Gpu(cudaStreamDestroy(stream));
}
void Measure() {
  constexpr unsigned width = 1920, height = 1080, frames = 100;
  void* device{};
  cudaEvent_t begin{}, end{};
  Gpu(cudaMalloc(&device, size_t(width) * height * 4));
  Gpu(cudaMemset(device, 0, size_t(width) * height * 4));
  Gpu(cudaEventCreate(&begin));
  Gpu(cudaEventCreate(&end));
  const d::analytics::ImageView image{device, size_t(width) * 4, width, height};
  Gpu(d::FinishOpaqueVideoCanvas(image, {0, 180, width, 720}, nullptr));
  Gpu(cudaEventRecord(begin));
  for (unsigned i = 0; i < frames; ++i)
    Gpu(d::FinishOpaqueVideoCanvas(image, {0, 180, width, 720}, nullptr));
  Gpu(cudaEventRecord(end));
  Gpu(cudaStreamSynchronize(nullptr));
  float elapsed = 0;
  Gpu(cudaEventElapsedTime(&elapsed, begin, end));
  std::cout << "Opaque canvas 1080p GPU milliseconds/frame=" << elapsed / frames << '\n';
  Gpu(cudaEventDestroy(begin));
  Gpu(cudaEventDestroy(end));
  Gpu(cudaFree(device));
}
} // namespace
int main() {
  try {
    Gpu(cudaSetDevice(0));
    Pixels();
    Measure();
    std::cout << "Opaque video canvas and transparent-corner blend passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
