#include "ScoreboardCuda.h"

#include "jetson-utils/cuda/cudaOverlay.h"

#include <cuda_runtime.h>

#include <array>
#include <cmath>
#include <iostream>

namespace {

bool check(cudaError_t result, const char* operation) {
  if (result == cudaSuccess)
    return true;
  std::cerr << operation << ": " << cudaGetErrorString(result) << '\n';
  return false;
}

bool close_to(unsigned actual, unsigned expected) {
  return std::abs(static_cast<int>(actual) - static_cast<int>(expected)) <= 2;
}

} // namespace

int main() {
  constexpr int kWidth = 2;
  constexpr int kHeight = 2;
  constexpr size_t kRowBytes = kWidth * sizeof(uchar4);
  uchar4* source = nullptr;
  uchar4* destination = nullptr;
  size_t source_pitch = 0;
  size_t destination_pitch = 0;
  cudaStream_t stream = nullptr;
  const bool allocated =
      check(cudaMallocPitch(reinterpret_cast<void**>(&source), &source_pitch, kRowBytes, kHeight), "source allocation") &&
      check(cudaMallocPitch(reinterpret_cast<void**>(&destination), &destination_pitch, kRowBytes, kHeight),
            "destination allocation") &&
      check(cudaStreamCreate(&stream), "stream creation");
  bool okay = allocated;
  if (okay) {
    const std::array<uchar4, 4> extracted = {
        make_uchar4(220, 180, 100, 0), make_uchar4(20, 40, 60, 0),
        make_uchar4(0, 0, 0, 0), make_uchar4(255, 255, 255, 0)};
    std::array<uchar4, 4> actual{};
    okay = check(cudaMemcpy2DAsync(source, source_pitch, extracted.data(), kRowBytes, kRowBytes, kHeight,
                                   cudaMemcpyHostToDevice, stream), "upload extracted ROI") &&
        check(hm::scoreboard::make_scoreboard_opaque(source, source_pitch, kWidth, kHeight, stream),
              "make extracted ROI opaque") &&
        check(cudaMemcpy2DAsync(actual.data(), kRowBytes, source, source_pitch, kRowBytes, kHeight,
                                cudaMemcpyDeviceToHost, stream), "download extracted ROI") &&
        check(cudaStreamSynchronize(stream), "synchronize extracted ROI");
    for (size_t i = 0; i < actual.size(); ++i)
      okay &= actual[i].x == extracted[i].x && actual[i].y == extracted[i].y &&
          actual[i].z == extracted[i].z && actual[i].w == 255;
    if (!okay)
      std::cerr << "Extracted scoreboard RGB or alpha changed unexpectedly\n";
  }
  if (okay) {
    // The half-covered pixel represents the warp's bilinear interpolation
    // against transparent black. The final overlay must apply alpha once.
    const std::array<uchar4, 4> warped = {
        make_uchar4(100, 75, 50, 128), make_uchar4(0, 0, 200, 255),
        make_uchar4(0, 0, 0, 0), make_uchar4(60, 30, 10, 64)};
    const std::array<uchar4, 4> background = {
        make_uchar4(0, 0, 0, 255), make_uchar4(0, 0, 0, 255),
        make_uchar4(0, 0, 0, 255), make_uchar4(0, 0, 0, 255)};
    std::array<uchar4, 4> actual{};
    okay = check(cudaMemcpy2DAsync(source, source_pitch, warped.data(), kRowBytes, kRowBytes, kHeight,
                                   cudaMemcpyHostToDevice, stream), "upload warped pixels") &&
        check(cudaMemcpy2DAsync(destination, destination_pitch, background.data(), kRowBytes, kRowBytes, kHeight,
                                cudaMemcpyHostToDevice, stream), "upload background") &&
        check(hm::scoreboard::unpremultiply_scoreboard(source, source_pitch, kWidth, kHeight, stream),
              "restore straight alpha") &&
        check(cudaOverlayPitch(source, kWidth, kHeight, source_pitch, destination, kWidth, kHeight,
                               destination_pitch, IMAGE_RGBA8, 0, 0, stream), "overlay scoreboard") &&
        check(cudaMemcpy2DAsync(actual.data(), kRowBytes, destination, destination_pitch, kRowBytes, kHeight,
                                cudaMemcpyDeviceToHost, stream), "download overlay") &&
        check(cudaStreamSynchronize(stream), "synchronize overlay");
    okay &= close_to(actual[0].x, 100) && close_to(actual[0].y, 75) && close_to(actual[0].z, 50) &&
        actual[0].w == 255 && actual[1].z == 200 && actual[2].x == 0 && actual[2].y == 0 && actual[2].z == 0 &&
        close_to(actual[3].x, 60) && close_to(actual[3].y, 30) && close_to(actual[3].z, 10);
    if (!okay)
      std::cerr << "Partially covered scoreboard pixels were blended twice\n";
  }
  if (stream)
    cudaStreamDestroy(stream);
  if (destination)
    cudaFree(destination);
  if (source)
    cudaFree(source);
  return okay ? 0 : 1;
}
