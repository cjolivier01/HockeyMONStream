#pragma once

#include "gstvideoprep.h"

#include "custom_algorithm_base.h"

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

#ifdef NEW_VIDEOPREP
using STITCH_PRIV_BASE = CustomAlgorithmBase;
#else
using STITCH_PRIV_BASE = hm::videoprep::VideoPrepPriv;
#endif


class PlayCropperPriv : public STITCH_PRIV_BASE {
  using Super = STITCH_PRIV_BASE;
 public:
  PlayCropperPriv(int gpu_id, size_t batch_size) : STITCH_PRIV_BASE(gpu_id, batch_size) {}

  // template <typename... Args>
  // void render(Args&&... args) {
  //     // Forward all arguments to the target function
  //     render_(std::forward<Args>(args)...);
  // }
 
  bool PreCapsInit(DSCustom_CreateParams* params) override;
  bool PostCapsInit(DSCustom_CreateParams* params) override;

  BufferResult ProcessBuffer(GstBuffer* inbuf) override;

  // bool render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
  //   return render_.render(name, surface, stream);
  // }

  // -DSCustomLibraryBase
  // bool SetProperty(const Property& prop) override {
  //   assert(false);
  //   return true;
  // }

  // bool HandleEvent(GstEvent* event) override {
  //   return true;
  // }

  // char* QueryProperties() override {
  //   assert(false);
  //   return strdup("");
  // }

  // BufferResult ProcessBuffer(GstBuffer* inbuf) override {
  //   assert(false);
  //   return BufferResult::Buffer_Ok;
  // }

  // DSCustomLibraryBase-

  cudaError GenerateOutput(
      NvDsBatchMeta* batch_meta,
      videoprep::GstVideoPrep* videoprep,
      NvBufSurface* in_surface,
      NvBufSurface* out_surface) override;

  gint AllocateScratchBuffers(videoprep::GstVideoPrep *videoprep) override;

 protected:
};

/** GStreamer boilerplate. */
struct GstVideoPrepPlayCropper : public videoprep::GstVideoPrep {};

struct GstVideoPrepPlayCropperClass : public videoprep::GstVideoPrepClass {};

} // namespace playcropper
} // namespace hm
