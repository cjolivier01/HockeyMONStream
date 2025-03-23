#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"
#include "hstream/src/libs/draw_display/Fonts.h"
#include "hstream/src/libs/scoreboard/Scoreboard.h"
#include "cupano/pano/cudaMat.h"

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

  absl::Status RenderDisplayMeta(surface::Surface surface, const NvDsFrameMeta* frame_meta, cudaStream_t stream);
  absl::Status RenderScoreboard(surface::Surface in_surface, surface::Surface out_surface, cudaStream_t stream);

  bool use_unfused_kernels_{false};
  bool show_{false};
  size_t frame_count_{0};
  std::shared_ptr<draw_display::FontCache> font_cache_;
  float render_scale_{0.5};
  std::unique_ptr<hm::CudaMat<uchar4>> display_surface_;
  bool show_scoreboard_{false};
  float scoreboard_width_ratio_{1.0/6};
  float scoreboard_height_ratio_{1.0/6};
  std::unique_ptr<hm::scoreboard::Scoreboard<uchar4>> scoreboard_;
  std::vector<cv::Point2f> scoreboard_perspective_polygion_;
  size_t scoreboard_warp_interval_{3};
  NvBufSurfaceParams display_dest_params_;
  bool plot_play_tracking_;
  bool plot_player_tracking_;
};

} // namespace playcropper
} // namespace hm
