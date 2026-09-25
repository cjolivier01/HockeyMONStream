#include "hstream/src/gst-plugins/gst-player-analytics/BorrowedImage.h"

#include <cuda_runtime_api.h>

#include <cstdint>
#include <cstdio>
#include <limits>

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
#include <cuda.h>
#include <cudaEGL.h>
#endif

namespace hm::player_analytics {

struct BorrowedImage::Import {
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  NvBufSurface* surface{nullptr};
  unsigned index{0};
  bool owns_image{false};
  CUgraphicsResource resource{nullptr};
  ~Import() {
    if (resource) {
      const auto result = cuGraphicsUnregisterResource(resource);
      if (result != CUDA_SUCCESS)
        std::fprintf(stderr, "Player analytics CUDA EGL unregister failed: %d\n", int(result));
    }
    if (owns_image && NvBufSurfaceUnMapEglImage(surface, index) != 0)
      std::fprintf(stderr, "Player analytics EGL image unmap failed\n");
  }
#endif
};

BorrowedImage::BorrowedImage() = default;
BorrowedImage::~BorrowedImage() = default;

absl::StatusOr<std::unique_ptr<BorrowedImage>> BorrowedImage::Map(NvBufSurface* surface, unsigned index, int gpu_id) {
  if (gpu_id < 0 || !surface || !surface->surfaceList || surface->numFilled > surface->batchSize ||
      index >= surface->numFilled || index >= surface->batchSize || surface->gpuId != static_cast<unsigned>(gpu_id))
    return absl::InvalidArgumentError("Player analytics requires a filled surface on its configured GPU");
  auto& params = surface->surfaceList[index];
  if (params.width == 0 || params.height == 0 || params.width > 65536 || params.height > 65536)
    return absl::InvalidArgumentError("Invalid player analytics surface dimensions");
  auto result = std::unique_ptr<BorrowedImage>(new BorrowedImage());
  auto& view = result->view_;
  view.width = static_cast<int>(params.width);
  view.height = static_cast<int>(params.height);
  switch (params.colorFormat) {
    case NVBUF_COLOR_FORMAT_RGBA:
      view.format = PixelFormat::kRgba8;
      break;
    case NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_709:
    case NVBUF_COLOR_FORMAT_RGBA_10_10_10_2_2020:
      view.format = PixelFormat::kRgba10A2;
      break;
    default:
      return absl::InvalidArgumentError("Player analytics supports RGBA8 and packed RGB10A2 NVMM surfaces");
  }
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  if (surface->memType == NVBUF_MEM_SURFACE_ARRAY || surface->memType == NVBUF_MEM_DEFAULT) {
    result->import_ = std::make_unique<Import>();
    auto& imported = *result->import_;
    imported.surface = surface;
    imported.index = index;
    if (!params.mappedAddr.eglImage) {
      if (NvBufSurfaceMapEglImage(surface, index) != 0)
        return absl::InternalError("Cannot map the player analytics EGL surface");
      imported.owns_image = true;
    }
    if (cuGraphicsEGLRegisterImage(
            &imported.resource, params.mappedAddr.eglImage, CU_GRAPHICS_MAP_RESOURCE_FLAGS_READ_ONLY) != CUDA_SUCCESS)
      return absl::InternalError("Cannot register the player analytics EGL surface with CUDA");
    CUeglFrame frame{};
    // EGL exposes aligned storage dimensions (for example 7136 for a 7135-pixel
    // canvas). Keep the logical extent in view; storage must cover it. Stitching
    // can also set planeParams dimensions to the logical extent.
    if (cuGraphicsResourceGetMappedEglFrame(&frame, imported.resource, 0, 0) != CUDA_SUCCESS ||
        frame.frameType != CU_EGL_FRAME_TYPE_PITCH || frame.planeCount != 1 || !frame.frame.pPitch[0] ||
        frame.width < params.width || frame.height < params.height)
      return absl::InvalidArgumentError("Player analytics requires a single pitched CUDA EGL plane");
    view.data = frame.frame.pPitch[0];
    view.pitch = frame.pitch;
  } else
#endif
  {
    if (surface->memType != NVBUF_MEM_CUDA_DEVICE && surface->memType != NVBUF_MEM_CUDA_UNIFIED &&
        surface->memType != NVBUF_MEM_DEFAULT)
      return absl::InvalidArgumentError("Player analytics requires device-resident NVMM memory");
    if (params.layout != NVBUF_LAYOUT_PITCH)
      return absl::InvalidArgumentError("Player analytics requires pitched CUDA memory");
    view.data = params.dataPtr;
    view.pitch = params.pitch;
    cudaPointerAttributes attributes{};
    if (!view.data || cudaPointerGetAttributes(&attributes, view.data) != cudaSuccess ||
        (attributes.type != cudaMemoryTypeDevice && attributes.type != cudaMemoryTypeManaged) ||
        (attributes.type == cudaMemoryTypeDevice && attributes.device != gpu_id))
      return absl::InvalidArgumentError("Player analytics surface does not reference memory on its CUDA device");
  }
  if (!view.data || view.pitch < static_cast<size_t>(view.width) * 4 || view.pitch % 4 != 0 ||
      reinterpret_cast<uintptr_t>(view.data) % alignof(uint32_t) != 0 ||
      view.pitch > std::numeric_limits<size_t>::max() / static_cast<size_t>(view.height))
    return absl::InvalidArgumentError("Invalid player analytics CUDA surface pitch");
  const size_t required_bytes = view.pitch * (view.height - 1) + static_cast<size_t>(view.width) * 4;
  if (params.dataSize && params.dataSize < required_bytes)
    return absl::InvalidArgumentError("Player analytics surface allocation is smaller than its pitched image");
  return result;
}

} // namespace hm::player_analytics
