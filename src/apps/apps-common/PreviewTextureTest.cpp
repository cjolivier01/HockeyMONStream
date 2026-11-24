#include "PreviewTexture.cuh"

#include <cuda_runtime.h>

#include <array>
#include <cstdio>
#include <cstdlib>

namespace {
void check(cudaError_t result, const char* operation) {
  if (result != cudaSuccess) {
    std::fprintf(stderr, "%s: %s\n", operation, cudaGetErrorString(result));
    std::exit(1);
  }
}
} // namespace

int main() {
  constexpr unsigned width = 17, height = 9;
  constexpr size_t row_bytes = width * sizeof(uchar4);
  constexpr size_t pitch = row_bytes + 1, offset = 1;
  std::array<unsigned char, offset + height * pitch> source{};
  std::array<unsigned char, height * row_bytes> expected{}, actual{};
  for (unsigned y = 0; y < height; ++y) {
    for (size_t i = 0; i < row_bytes; ++i) {
      const auto value = static_cast<unsigned char>(y * 23 + i);
      source[offset + y * pitch + i] = value;
      expected[y * row_bytes + i] = value;
    }
  }

  unsigned char* device = nullptr;
  check(cudaMalloc(&device, source.size()), "allocate source");
  check(cudaMemcpy(device, source.data(), source.size(), cudaMemcpyHostToDevice), "upload source");
  const auto format = cudaCreateChannelDesc<uchar4>();
  cudaArray_t array = nullptr;
  check(cudaMallocArray(&array, &format, width, height, cudaArraySurfaceLoadStore), "allocate destination");
  cudaResourceDesc resource{};
  resource.resType = cudaResourceTypeArray;
  resource.res.array.array = array;
  cudaSurfaceObject_t destination{};
  check(cudaCreateSurfaceObject(&destination, &resource), "create surface");
  check(hm::gpu_preview::WritePreviewTexture(destination, device + offset, pitch, width, height, nullptr), "write");
  check(cudaStreamSynchronize(nullptr), "finish write");
  check(cudaDestroySurfaceObject(destination), "destroy surface");
  check(cudaMemcpy2DFromArray(actual.data(), row_bytes, array, 0, 0, row_bytes, height, cudaMemcpyDeviceToHost), "read");
  check(cudaFreeArray(array), "free destination");
  check(cudaFree(device), "free source");
  if (expected != actual) {
    std::fputs("Preview texture differs from the pitched RGBA source\n", stderr);
    return 1;
  }
  return 0;
}
