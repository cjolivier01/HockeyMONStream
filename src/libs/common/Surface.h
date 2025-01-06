#pragma once

#include "nvbufsurface.h"

#include "hockeymom/csrc/play_tracker/BoxUtils.h"

#include "nvdsmeta.h"

#include <memory>
#include <vector>

namespace hm {
namespace surface {

class Surface {
 public:
  Surface(NvBufSurfaceParams* params) : params_(params) {}

  constexpr void* dataptr() {
    return params_->dataPtr;
  }
  constexpr guint width() const {
    return params_->width;
  }
  constexpr guint height() const {
    return params_->height;
  }
  constexpr guint pitch() const {
    return params_->pitch;
  }
  constexpr const NvBufSurfaceParams* get() const {
    return params_;
  }
  constexpr const NvBufSurfaceParams* operator->() const {
    return get();
  }

 private:
  NvBufSurfaceParams* params_{nullptr};
};

class SurfaceList {
 public:
  SurfaceList(int gpu_id, size_t batch_size);
  SurfaceList(const NvBufSurface* buf_surface);

  constexpr operator NvBufSurface*() {
    return &nv_surf_;
  }
  constexpr operator const NvBufSurface*() const {
    return &nv_surf_;
  }
  size_t size() const {
    return nv_surface_list_.size();
  };
  Surface operator[](size_t idx) {
    return Surface(&nv_surface_list_.at(idx));
  }
  void add_surface(void* data, size_t width, size_t height, size_t pitch, size_t bytes_per_pixes = 4);
  void add_surface(const NvBufSurfaceParams* surface_params);
  void add_surface(const Surface& surface);

 private:
  NvBufSurface nv_surf_;
  std::vector<NvBufSurfaceParams> nv_surface_list_;
};

} // namespace surface
} // namespace hm
