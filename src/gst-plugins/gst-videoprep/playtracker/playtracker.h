#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/libs/common/Status.h"

namespace hm {
namespace playtracker {

class PlayTrackerPriv : public CustomAlgorithmBase {
  using Super = CustomAlgorithmBase;

 public:
  PlayTrackerPriv(int gpu_id, size_t batch_size) : CustomAlgorithmBase(gpu_id, batch_size) {}
  ~PlayTrackerPriv();

  absl::Status PreCapsInit(DSCustom_CreateParams* params) override;
  absl::Status PostCapsInit(DSCustom_CreateParams* params) override;

  // -DSCustomLibraryBase
  BufferResult ProcessBuffer(GstBuffer* inbuf) override;
  bool SetProperty(const Property& prop) override;
  // DSCustomLibraryBase-

  absl::Status GenerateOutput(NvDsBatchMeta* batch_meta, NvBufSurface* in_surface, NvBufSurface* out_surface) override;

 protected:
  DsPlayTrackerInitParams init_params_;
  DsPlayTrackerCtx* pt_context_{nullptr};
  bool show_{false};
};

} // namespace playtracker
} // namespace hm
