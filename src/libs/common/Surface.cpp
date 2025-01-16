#include "src/libs/common/Surface.h"

#include <npp.h>

#include <cassert>
#include <iostream>

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

namespace hm {
namespace surface {

SurfaceList::SurfaceList(int gpu_id, size_t batch_size) {
  memset(&nv_surf_, 0, sizeof(nv_surf_));
  nv_surf_.gpuId = gpu_id;
  nv_surf_.batchSize = batch_size;
  nv_surf_.numFilled = 1;
  nv_surf_.memType = NVBUF_MEM_CUDA_DEVICE;
  nv_surf_.surfaceList = nullptr;
  // Just a guess on final size as batch size
  nv_surface_list_.reserve(batch_size);
}

SurfaceList::SurfaceList(const NvBufSurface* buf_surface) {
  memcpy(&nv_surf_, buf_surface, sizeof(nv_surf_));
  // which one?
  assert(buf_surface->numFilled == buf_surface->batchSize);
  for (size_t i = 0; i < buf_surface->batchSize; ++i) {
    add_surface(&buf_surface->surfaceList[i]);
  }
}

SurfaceList::~SurfaceList() {
  clear();
}

void SurfaceList::clear() {
  for (size_t i = 0, n = nv_surface_list_.size(); i < n; ++i) {
    if (owns_.at(i)) {
      if (nv_surface_list_[i].dataPtr) {
        cudaFree(nv_surface_list_[i].dataPtr);
      }
    }
  }
  nv_surface_list_.clear();
  owns_.clear();
}

void SurfaceList::add_surface(
    void* data,
    size_t width,
    size_t height,
    size_t pitch,
    size_t bytes_per_pixes,
    bool owns) {
  NvBufSurfaceParams& params = nv_surface_list_.emplace_back();
  owns_.emplace_back(owns);
  memset(&params, 0, sizeof(params));

  NppiSize inSrcSize = {(gint)width, (gint)height};

  params.pitch = pitch;
  params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  params.width = inSrcSize.width;
  params.height = inSrcSize.height;
  params.planeParams.num_planes = 1;
  params.planeParams.width[0] = inSrcSize.width;
  params.planeParams.height[0] = inSrcSize.height;
  params.planeParams.pitch[0] = params.pitch;
  params.planeParams.psize[0] = inSrcSize.height * params.pitch;
  params.planeParams.bytesPerPix[0] = bytes_per_pixes;

  params.dataSize = params.planeParams.psize[0];
  params.dataPtr = (guint*)data;
  params.layout = NVBUF_LAYOUT_PITCH;

  // Reset the surface-list pointer
  nv_surf_.surfaceList = &nv_surface_list_[0];
}

void SurfaceList::add_surface(const NvBufSurfaceParams* surface_params) {
  NvBufSurfaceParams& params = nv_surface_list_.emplace_back();
  // We never own these right now
  owns_.emplace_back(false);
  memcpy(&params, surface_params, sizeof(params));
  // Reset the surface-list pointer
  nv_surf_.surfaceList = &nv_surface_list_[0];
}

void SurfaceList::add_surface(const Surface& surface) {
  add_surface(surface.get());
}

EglSurface::EglSurface(NvBufSurface* surface, int index) : surface_(surface), index_(index) {}
~EglSurface::EglSurface() {
  unmap();
}

cudaError_t EglSurface::map() {
  auto nv_error = NvBufSurfaceMapEglImage(surface_, index_);
  if (nv_error != 0) {
    std::cerr << "FAILED!!!" << std::endl;
    assert(false);
    return cudaSuccess;
  }

  auto egl_image = surface_->surfaceList[index_].mappedAddr.eglImage;

  cudaGraphicsResource* cuResource{nullptr};
  cudaError_t cuerr_result = cudaGraphicsEGLRegisterImage(&cuResource, egl_image, CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
  if (cuerr_result != cudaSuccess) {
    std::cerr << "FAILED!!!" << std::endl;
    assert(false);
    return cuerr_result;
  }

  cuerr_result = cudaGraphicsResourceGetMappedEglFrame(&eglFrame_, cuResource, 0, 0);
  if (cuerr_result != cudaSuccess) {
    std::cerr << "FAILED!!!" << std::endl;
    assert(false);
    return cuerr_result;
  }
#if 1
  // void* array_memory = eglFrame_.frame.pArray[0];
  pitch_memory_ = eglFrame_.frame.pPitch[0];
  // test write
  cuerr_result = cudaMemset(pitch_memory_.ptr, 128, pitch_memory_.ysize * pitch_memory_.pitch);
  if (cuerr_result != cudaSuccess) {
    std::cerr << "FAILED!!!" << std::endl;
    assert(false);
    return cuerr_result;
  }
#endif
  return cudaSuccess;
}

cudaError_t EglSurface::unmap() {
  if (cuResource) {
    cuerr_result = cudaGraphicsUnregisterResource(cuResource);
    if (cuerr_result != cudaSuccess) {
      std::cerr << "FAILED!!!" << std::endl;
      assert(false);
      return cuerr_result;
    }
    cuResource = nullptr;
  }
  if (surface_->surfaceList[index_].mappedAddr.eglImage) {
    /* Destroy the EGLImage */
    auto nvresult = NvBufSurfaceUnMapEglImage(surface_, index_);
    if (nvresult != 0) {
      std::cerr << "FAILED!!!" << std::endl;
      assert(false);
      return cudaErrorAssert;
    }
  }
  memset(&pitch_memory_, 0, sizeof(pitch_memory_));
}

} // namespace surface
} // namespace hm
