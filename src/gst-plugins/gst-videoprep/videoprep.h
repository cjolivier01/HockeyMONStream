#pragma once

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
 * Function definition of dewarping call for each surface.
 *
 * @param[in] videoprep Width of the network input, in pixels.
 * @param[in] in_surface Height of the network input, in pixels.
 * @param[in] out_surface Color format of the buffers in the pool.
 *
 * @return Cuda Error. "cudaSuccess" in case of Success.
 */
cudaError gst_videoprep_do_dewarp(NvDsBatchMeta* batch_meta, GstVideoPrep* videoprep, NvBufSurface* in_surface, NvBufSurface* out_surface);

/**
 * Function to get core Dewarper library version.
 *
 * @return  The version number.
 */
uint32_t gst_videoprep_version();

std::vector<hm::BBox> get_tracking_boxes(NvDsBatchMeta* batch_meta);

} // namespace videoprep
} // namespace hm
