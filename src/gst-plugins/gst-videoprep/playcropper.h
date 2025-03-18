#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"
#include "hstream/src/libs/common/Status.h"

namespace hm {
namespace playcropper {

/**
 * @addtogroup three Standard GStreamer boilerplate
 * @{
 */
#define GST_TYPE_PLAY_CROPPER (hm::videoprep::gst_videoprep_get_type())
#define GST_VIDEOPREP_PLAY_CROPPER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PLAY_CROPPER, GstVideoPrepPlayCropper))
#define GST_VIDEOPREP_PLAY_CROPPER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PLAY_CROPPER, GstVideoPrepPlayCropperClass))
#define GST_IS_VIDEOPREP_PLAY_CROPPER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PLAY_CROPPER))
#define GST_IS_VIDEOPREP_PLAY_CROPPER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PLAY_CROPPER))

using STITCH_PRIV_BASE = CustomAlgorithmBase;

class PlayCropperPriv : public STITCH_PRIV_BASE {
  using Super = STITCH_PRIV_BASE;

 public:
  PlayCropperPriv(int gpu_id, size_t batch_size) : STITCH_PRIV_BASE(gpu_id, batch_size) {}

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

} // namespace playcropper
} // namespace hm
