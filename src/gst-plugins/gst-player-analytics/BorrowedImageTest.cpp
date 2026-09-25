// Jetson NVMM import regression. Only four sentinel bytes are read back per
// case; production imports remain GPU-resident and never copy the frame.
#include "hstream/src/gst-plugins/gst-player-analytics/BorrowedImage.h"

#include <cuda_runtime_api.h>

#include <cstdint>
#include <iostream>

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
#include <cuda.h>
#include <cudaEGL.h>

namespace {
namespace pa = hm::player_analytics;

bool Check(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}

struct Surface {
  NvBufSurface* surface{nullptr};
  ~Surface() {
    if (surface) {
      if (surface->surfaceList[0].mappedAddr.eglImage)
        NvBufSurfaceUnMapEglImage(surface, 0);
      NvBufSurfaceDestroy(surface);
    }
  }
  bool Create(unsigned width, unsigned height, NvBufSurfaceLayout layout = NVBUF_LAYOUT_PITCH) {
    NvBufSurfaceCreateParams params{};
    params.gpuId = 0;
    params.width = width;
    params.height = height;
    params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    params.layout = layout;
    params.memType = NVBUF_MEM_SURFACE_ARRAY;
    if (!Check(NvBufSurfaceCreate(&surface, 1, &params) == 0, "Cannot create test NVMM surface"))
      return false;
    surface->numFilled = 1;
    return true;
  }
};

bool Pitched(unsigned width, unsigned height, bool premapped, bool logical_plane_dimensions) {
  Surface fixture;
  if (!fixture.Create(width, height) ||
      !Check(NvBufSurfaceMemSet(fixture.surface, 0, -1, 127) == 0, "Cannot initialize sentinel surface"))
    return false;
  auto& params = fixture.surface->surfaceList[0];
  if (logical_plane_dimensions) {
    params.planeParams.width[0] = width;
    params.planeParams.height[0] = height;
  }
  if (premapped && !Check(NvBufSurfaceMapEglImage(fixture.surface, 0) == 0, "Cannot premap test EGL image"))
    return false;
  const auto original_egl_image = params.mappedAddr.eglImage;
  bool okay = true;
  {
    auto borrowed = pa::BorrowedImage::Map(fixture.surface, 0, 0);
    if (!borrowed.ok()) {
      std::cerr << "Pitched " << width << 'x' << height << " premapped=" << premapped
                << " logical-plane=" << logical_plane_dimensions << ": " << borrowed.status() << '\n';
      return false;
    }
    const auto& view = (*borrowed)->view();
    okay &= Check(
        view.width == static_cast<int>(width) && view.height == static_cast<int>(height),
        "Import exposed storage padding as logical pixels");
    okay &= Check(
        view.format == pa::PixelFormat::kRgba8 && view.pitch >= width * 4 && view.pitch == params.pitch,
        "Import changed pixel format or pitch");
    const auto* corner = static_cast<const uint8_t*>(view.data) + view.pitch * (height - 1) + 4 * (width - 1);
    uint32_t pixel = 0;
    // Synchronous, bounded test readback completes before the borrowed import
    // is destroyed, satisfying its stream/lifetime contract.
    okay &= Check(
        cudaMemcpy(&pixel, corner, sizeof(pixel), cudaMemcpyDeviceToHost) == cudaSuccess,
        "Cannot read logical bottom-right pixel from imported GPU surface");
    okay &= Check(pixel == 0x7f7f7f7fU, "Imported logical bottom-right pixel differs from sentinel");
  }
  okay &= Check(params.mappedAddr.eglImage == original_egl_image, "Import changed caller EGL mapping ownership");
  // Re-import the same allocation after unregister/unmap to exercise reuse.
  {
    auto borrowed = pa::BorrowedImage::Map(fixture.surface, 0, 0);
    okay &= Check(borrowed.ok(), "Cannot re-import the same GPU surface");
  }
  okay &= Check(params.mappedAddr.eglImage == original_egl_image, "Re-import changed caller EGL mapping ownership");
  if (okay)
    std::cout << "PASS pitched " << width << 'x' << height << " premapped=" << premapped
              << " logical-plane=" << logical_plane_dimensions << '\n';
  return okay;
}

bool RejectUndersizedStorage() {
  Surface fixture;
  if (!fixture.Create(321, 241) ||
      !Check(NvBufSurfaceMapEglImage(fixture.surface, 0) == 0, "Cannot map undersized-storage fixture"))
    return false;
  auto& params = fixture.surface->surfaceList[0];
  const auto original_egl_image = params.mappedAddr.eglImage;
  CUgraphicsResource resource = nullptr;
  if (!Check(
          cuGraphicsEGLRegisterImage(&resource, original_egl_image, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY) ==
              CUDA_SUCCESS,
          "Cannot inspect undersized-storage fixture"))
    return false;
  CUeglFrame frame{};
  const auto mapped = cuGraphicsResourceGetMappedEglFrame(&frame, resource, 0, 0);
  const auto unregistered = cuGraphicsUnregisterResource(resource);
  if (!Check(mapped == CUDA_SUCCESS && unregistered == CUDA_SUCCESS, "Cannot read storage extent"))
    return false;
  bool okay = true;
  const unsigned logical_width = params.width;
  const unsigned logical_height = params.height;
  params.width = frame.width + 1;
  okay &= Check(
      absl::IsInvalidArgument(pa::BorrowedImage::Map(fixture.surface, 0, 0).status()),
      "Import accepted a logical width beyond EGL storage");
  params.width = logical_width;
  params.height = frame.height + 1;
  okay &= Check(
      absl::IsInvalidArgument(pa::BorrowedImage::Map(fixture.surface, 0, 0).status()),
      "Import accepted a logical height beyond EGL storage");
  params.height = logical_height;
  const auto allocation_size = params.dataSize;
  params.dataSize = params.pitch * (params.height - 1) + params.width * 4 - 1;
  okay &= Check(
      absl::IsInvalidArgument(pa::BorrowedImage::Map(fixture.surface, 0, 0).status()),
      "Import accepted an allocation smaller than its logical pitched image");
  params.dataSize = allocation_size;
  okay &= Check(params.mappedAddr.eglImage == original_egl_image, "Failed import released a caller-owned EGL image");
  if (okay)
    std::cout << "PASS undersized storage/allocation rejection and failed-import ownership\n";
  return okay;
}

bool RejectArray() {
  Surface fixture;
  if (!fixture.Create(320, 240, NVBUF_LAYOUT_BLOCK_LINEAR))
    return false;
  const bool rejected = absl::IsInvalidArgument(pa::BorrowedImage::Map(fixture.surface, 0, 0).status());
  bool okay = Check(rejected, "Import accepted an unsupported block-linear CUDA array");
  okay &= Check(!fixture.surface->surfaceList[0].mappedAddr.eglImage, "Failed import leaked its EGL image");
  if (okay)
    std::cout << "PASS CUDA-array rejection and failed-import cleanup\n";
  return okay;
}
} // namespace
#endif

int main() {
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  if (!Check(cudaSetDevice(0) == cudaSuccess && cudaFree(nullptr) == cudaSuccess, "Cannot initialize CUDA"))
    return 1;
  bool okay = true;
  okay &= Pitched(320, 240, false, false);
  okay &= Pitched(321, 241, false, false);
  okay &= Pitched(7135, 2634, false, false);
  okay &= Pitched(7135, 2634, true, false);
  okay &= Pitched(7135, 2634, false, true);
  okay &= Pitched(7135, 2634, true, true);
  okay &= RejectUndersizedStorage();
  okay &= RejectArray();
  return okay ? 0 : 1;
#else
  std::cout << "SKIP: Jetson EGL surface-array regression requires a Jetson GPU\n";
  return 0;
#endif
}
