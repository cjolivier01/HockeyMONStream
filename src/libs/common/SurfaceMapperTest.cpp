#include "Surface.h"

#include <array>
#include <iostream>
#include <memory>

int main() {
  for (auto memory : {NVBUF_MEM_CUDA_DEVICE, NVBUF_MEM_SURFACE_ARRAY}) {
    for (auto format : {NVBUF_COLOR_FORMAT_RGBA, NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709}) {
      // Orin cannot register packed 10-bit EGL images. The stitcher receives
      // that format in CUDA storage, while ordinary RGBA retains both paths.
      if (memory == NVBUF_MEM_SURFACE_ARRAY && format != NVBUF_COLOR_FORMAT_RGBA)
        continue;
      NvBufSurfaceCreateParams params{};
      params.width = 64;
      params.height = 32;
      params.layout = NVBUF_LAYOUT_PITCH;
      params.colorFormat = format;
      params.memType = memory;
      NvBufSurface* allocated = nullptr;
      if (NvBufSurfaceCreate(&allocated, 1, &params))
        return 1;
      std::unique_ptr<NvBufSurface, decltype(&NvBufSurfaceDestroy)> buffer(allocated, NvBufSurfaceDestroy);
      buffer->numFilled = 1;
      for (bool read_only : {false, true}) {
        hm::surface::EglSurfaceMapper mapper(buffer.get(), 0, read_only);
        if (!mapper.ok()) {
          std::cerr << "Mapping failed: " << cudaGetErrorString(mapper.status()) << '\n';
          return 1;
        }
        auto surface = mapper.get_surface();
        if (surface.width() != 64 || surface.height() != 32 || surface->colorFormat != format)
          return 1;
        if (memory == NVBUF_MEM_CUDA_DEVICE && surface.dataptr() != buffer->surfaceList[0].dataPtr)
          return 1;
        if (!read_only) {
          if (cudaMemset2D(surface.dataptr(), surface.pitch(), 0x6d, 64 * 4, 32) != cudaSuccess)
            return 1;
        } else {
          std::array<unsigned char, 64 * 4 * 32> pixels{};
          if (cudaMemcpy2D(
                  pixels.data(), 64 * 4, surface.dataptr(), surface.pitch(), 64 * 4, 32, cudaMemcpyDeviceToHost) !=
              cudaSuccess)
            return 1;
          for (auto pixel : pixels)
            if (pixel != 0x6d)
              return 1;
        }
      }
    }
  }
  std::cout << "CUDA-device and EGL surface mapping passed\n";
}
