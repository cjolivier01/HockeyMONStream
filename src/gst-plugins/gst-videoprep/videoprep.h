#pragma once

#include <nppi.h>

#include <iostream>

#include "gstvideoprep.h"
#include "nvbufsurface.h"

#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"

inline bool CUDA_CHECK_(gint e, gint iLine, const gchar* szFile) {
  if (e != cudaSuccess) {
    std::cout << "CUDA runtime error " << e << " at line " << iLine << " in file " << szFile << std::endl;
    return false;
  }
  return true;
}

#define cuda_ck(call) CUDA_CHECK_(call, __LINE__, __FILE__)

#define BAIL_IF_FALSE(x, err, code) \
  do {                              \
    if (!(x)) {                     \
      err = code;                   \
      goto bail;                    \
    }                               \
  } while (0)
namespace hm {
namespace videoprep {

/**
 * Function to get core Dewarper library version.
 *
 * @return  The version number.
 */
uint32_t gst_videoprep_version();

std::vector<hm::BBox> get_tracking_boxes(NvDsBatchMeta* batch_meta);

NppStatus cropSurface(
    const hm::surface::Surface& in_surface,
    const hm::BBox& src_rect,
    hm::surface::Surface out_surface,
    bool clear_output_surface,
    const NppStreamContext& nppStreamContext);

NppStatus rotateNvBufSurfaceWithNPP(
    const hm::surface::Surface& in_surface,
    const hm::BBox& src_rect,
    hm::surface::Surface out_surface,
    const hm::BBox& dest_rect,
    float angleDegrees,
    const Point& anchor_point,
    const NppStreamContext& nppStreamContext);

NppStatus cropAndResizeNvBufSurface(
    const hm::surface::Surface& in_surface,
    const BBox& src_rect,
    hm::surface::Surface out_surface,
    const BBox& dest_rect,
    const NppStreamContext& nppStreamContext);

template <typename T>
inline NppiSize get_nppisize(const T& box) {
  return NppiSize{.width = (int)box.width(), .height = (int)box.height()};
}

inline NppiRect get_nppirect(const hm::surface::Surface& surface) {
  return NppiRect{.x = 0, .y = 0, .width = (int)surface.width(), .height = (int)surface.height()};
}

template <typename T>
inline NppiRect get_nppirect(const T& box) {
  return NppiRect{.x = (int)box.left, .y = (int)box.top, .width = (int)box.width(), .height = (int)box.height()};
}

} // namespace videoprep
} // namespace hm
