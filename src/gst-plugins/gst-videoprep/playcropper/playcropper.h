#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"

namespace hm {
namespace playcropper {

class PlayCropperPriv : public CustomAlgorithmBase {
  using Super = CustomAlgorithmBase;

 public:
  PlayCropperPriv(int gpu_id, size_t batch_size) : CustomAlgorithmBase(gpu_id, batch_size) {}

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

} // namespace playcropper
} // namespace hm
