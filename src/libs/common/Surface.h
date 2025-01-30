/**
 * @file Surface.h
 * @brief Implements a container for Surface objects with round-robin iteration capabilities
 */
#pragma once

#include "nvbufsurface.h"

#include "hockeymom/csrc/play_tracker/BoxUtils.h"

#include "nvdsmeta.h"

#include <cuda_egl_interop.h>
#include <cuda_runtime.h>

#include <memory>
#include <vector>

namespace hm {
namespace surface {

class Surface {
 public:
  Surface(NvBufSurfaceParams* params) : params_(params) {
    static_assert(sizeof(*this) == sizeof(NvBufSurfaceParams*));
  }

  template <typename T = void*>
  constexpr T dataptr() {
    return static_cast<T>(params_->dataPtr);
  }
  template <typename T = void*>
  constexpr const T dataptr() const {
    return static_cast<const T>(params_->dataPtr);
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
  constexpr guint bytes_per_pixel() const {
    switch (params_->colorFormat) {
      case NVBUF_COLOR_FORMAT_RGBA:
        return 4;
      default:
        assert(false);
    }
  }
  constexpr guint pitch_width() const {
    assert(pitch() % bytes_per_pixel() == 0);
    guint pw = pitch() / bytes_per_pixel();
    assert(pw >= width());
    return pw;
  }
  constexpr guint size(bool assert_pitch = false) const {
    assert(!assert_pitch || pitch() == width() * bytes_per_pixel());
    return pitch() * height();
  }
  BBox bbox() const {
    return BBox(0, 0, width(), height());
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

/**
 * @class SurfaceList
 * @brief A container class that stores and manages Surface objects with round-robin iteration support
 *
 * SurfaceList provides an STL-compatible container interface for storing Surface objects.
 * It features a custom round-robin iterator that allows for infinite traversal of the contained
 * surfaces, automatically wrapping back to the beginning after reaching the end.
 *
 */
class SurfaceList {
 public:
  // Forward declaration of iterator
  class round_robin_iterator;

  SurfaceList(int gpu_id, size_t batch_size);
  SurfaceList(const NvBufSurface* buf_surface);
  ~SurfaceList();

  constexpr operator NvBufSurface*() {
    return &nv_surf_;
  }
  constexpr operator const NvBufSurface*() const {
    return &nv_surf_;
  }
  size_t size() const {
    return nv_surface_list_.size();
  }
  size_t empty() const {
    return nv_surface_list_.empty();
  }
  void clear();
  Surface operator[](size_t idx) {
    return Surface(&nv_surface_list_.at(idx));
  }
  void add_surface(
      void* data,
      size_t width,
      size_t height,
      size_t pitch,
      size_t bytes_per_pixel = 4,
      bool owns = false);
  void add_surface(const NvBufSurfaceParams* surface_params);
  void add_surface(const Surface& surface);

  /**
   * @class round_robin_iterator
   * @brief Custom iterator providing infinite traversal of the SurfaceList
   *
   * This iterator implements the forward iterator concept and provides round-robin
   * traversal behavior. When reaching the end of the container, it automatically
   * wraps around to the beginning. The iterator will never equal end() unless the
   * container is empty.
   */
  class round_robin_iterator {
   private:
    SurfaceList* container;
    size_t current_index;

   public:
    // Iterator traits
    using iterator_category = std::forward_iterator_tag;
    using value_type = Surface;
    using difference_type = std::ptrdiff_t;
    using pointer = Surface;
    using reference = Surface;

    // Constructor
    explicit round_robin_iterator(SurfaceList* list, size_t index = 0) : container(list), current_index(index) {}

    // Dereference operator
    reference operator*() {
      if (container->empty()) {
        throw std::runtime_error("Cannot dereference iterator of empty container");
      }
      return (*container)[current_index % container->size()];
    }

    // Arrow operator
    pointer operator->() {
      if (container->empty()) {
        throw std::runtime_error("Cannot dereference iterator of empty container");
      }
      return (*container)[current_index % container->size()];
    }

    // Pre-increment
    round_robin_iterator& operator++() {
      if (!container->empty()) {
        ++current_index;
      }
      return *this;
    }

    // Post-increment
    round_robin_iterator operator++(int) {
      round_robin_iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    // Equality operators
    bool operator==(const round_robin_iterator& other) const {
      // Will always return false unless container is empty
      return container->empty() || (container == other.container && current_index == other.current_index);
    }

    bool operator!=(const round_robin_iterator& other) const {
      return !(*this == other);
    }
  };
  // STL-like container interface
  round_robin_iterator begin() {
    return round_robin_iterator(this);
  }

  round_robin_iterator end() {
    // This iterator will never actually be reached in comparisons
    // unless the container is empty
    return round_robin_iterator(this, size());
  }

 private:
  NvBufSurface nv_surf_;
  std::vector<NvBufSurfaceParams> nv_surface_list_;
  // Whether we own the surface
  std::vector<bool> owns_;
};

#ifdef __aarch64__  /* Jetson */
class EglSurfaceMapper {
 public:
  EglSurfaceMapper(NvBufSurface* surface, int index);
  ~EglSurfaceMapper();

  Surface get_surface() {
    assert(surface_list_);
    return (*surface_list_)[0];
  }

 private:
  cudaError_t map();
  cudaError_t unmap();
  NvBufSurface* surface_;
  int index_;
  cudaGraphicsResource* cuResource_{nullptr};
  cudaEglFrame eglFrame_{
      0,
  };
  cudaPitchedPtr pitch_memory_{
      0,
  };
  std::unique_ptr<SurfaceList> surface_list_;
};
#endif // __aarch64__

} // namespace surface
} // namespace hm
