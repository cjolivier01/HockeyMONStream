#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"
#include "hstream/src/libs/common/Status.h"

namespace hm {
namespace playtracker {

class PlayTrackerPriv : public CustomAlgorithmBase {
  using Super = CustomAlgorithmBase;

 public:
  PlayTrackerPriv(int gpu_id, size_t batch_size) : CustomAlgorithmBase(gpu_id, batch_size) {}

  absl::Status PreCapsInit(DSCustom_CreateParams* params) override;
  absl::Status PostCapsInit(DSCustom_CreateParams* params) override;

  // -DSCustomLibraryBase
  BufferResult ProcessBuffer(GstBuffer* inbuf) override;
  bool SetProperty(const Property& prop) override;
  // DSCustomLibraryBase-

  absl::Status GenerateOutput(NvDsBatchMeta* batch_meta, NvBufSurface* in_surface, NvBufSurface* out_surface) override;

  gint AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) override;

 protected:
  bool show_{false};
};

/** GStreamer boilerplate. */
#if 0
struct GstVideoPrepPlayCropper : public videoprep::GstVideoPrep {
  // Don't add stuff here
  GstVideoPrepPlayCropper() {
    static_assert(sizeof(GstVideoPrepPlayCropper) == sizeof(videoprep::GstVideoPrep));
  }
};

struct GstVideoPrepPlayCropperClass : public videoprep::GstVideoPrepClass {
  GstVideoPrepPlayCropperClass() {
    static_assert(sizeof(GstVideoPrepPlayCropperClass) == sizeof(videoprep::GstVideoPrepClass));
  }
};
#endif

} // namespace playtracker
} // namespace hm
