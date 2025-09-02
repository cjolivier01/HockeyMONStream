#pragma once

#include "absl/status/statusor.h"
#include "hstream/src/libs/common/Status.h"

#include "cupano/pano/cudaPano.h"

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"

#include <mutex>

namespace hm {
namespace stitcher {

using STITCH_PRIV_BASE = CustomAlgorithmBase;

class StitcherPriv : public STITCH_PRIV_BASE {
  using Super = STITCH_PRIV_BASE;

 public:
  StitcherPriv(int gpu_id, size_t batch_size) : STITCH_PRIV_BASE(gpu_id, batch_size) {}
  ~StitcherPriv();

  bool render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
    return render_.render(name, surface, stream);
  }

  // -DSCustomLibraryBase

  absl::Status PreCapsInit(DSCustom_CreateParams* params) override;
  absl::Status PostCapsInit(DSCustom_CreateParams* params) override;

  bool SetProperty(const Property& prop) override;

  bool HandleEvent(GstEvent* event) override {
    return true;
  }

  char* QueryProperties() override {
    assert(false);
    return strdup("");
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override {
    return Super::ProcessBuffer(inbuf);
  }

  // DSCustomLibraryBase-

  absl::Status GenerateOutput(NvDsBatchMeta* batch_meta, NvBufSurface* in_surface, NvBufSurface* out_surface) override;

 private:
  using STITCHER = hm::pano::cuda::CudaStitchPano<uchar4, float4>;

  absl::StatusOr<STITCHER*> get_stitcher();

  absl::Mutex stitcher_mu_;
  std::unique_ptr<STITCHER> stitcher_ ABSL_GUARDED_BY(stitcher_mu_);
  std::string config_file_;
  std::mutex process_mu_;
  size_t process_pass_{0};
  bool configure_only_{false};
  size_t left_frame_offset_ns_{0}, right_frame_offset_ns_{0};
  bool show_{false};
};

} // namespace stitcher
} // namespace hm
