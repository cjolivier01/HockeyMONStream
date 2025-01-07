#pragma once

#include <nppi.h>

#include <optional>

#include "gstvideoprep.h"
#include "nvbufsurface.h"

#include "external/hm/hockeymom/csrc/play_tracker/BoxUtils.h"

inline bool CUDA_CHECK_(gint e, gint iLine, const gchar* szFile) {
  if (e != cudaSuccess) {
    std::cout << "CUDA runtime error " << e << " at line " << iLine << " in file " << szFile << endl;
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

} // namespace videoprep
} // namespace hm
