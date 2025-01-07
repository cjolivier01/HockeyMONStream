#include "libs/common/Surface.h"

#include <npp.h>

#include <cassert>

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
    nv_surface_list_.clear();
    owns_.clear();
  }
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

} // namespace surface
} // namespace hm
