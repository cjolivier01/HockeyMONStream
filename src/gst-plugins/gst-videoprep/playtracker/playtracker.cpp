#include "playtracker.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include "deepstream/sources/includes/nvbufsurface.h"
#include <cmath>
#include "deepstream/sources/includes/nvbufsurface.h"
#include "nvdsmeta.h"

#include <assert.h>
#include <cuda.h>
#include <unistd.h>

namespace hm {
namespace playtracker {
namespace {} // namespace

absl::Status PlayTrackerPriv::PreCapsInit(DSCustom_CreateParams* params) {
  // Not an in-place transform
  m_inVideoFmt = GST_VIDEO_FORMAT_RGBA;
  m_outVideoFmt = GST_VIDEO_FORMAT_RGBA;
  return Super::PreCapsInit(params);
};

absl::Status PlayTrackerPriv::PostCapsInit(DSCustom_CreateParams* params) {
  m_transformMode = true;
  return Super::PostCapsInit(params);
}

bool PlayTrackerPriv::SetProperty(const Property& prop) {
  if (prop.key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  }
  return true;
}

BufferResult PlayTrackerPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

absl::Status PlayTrackerPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  return absl::OkStatus();
}

} // namespace playtracker
} // namespace hm
