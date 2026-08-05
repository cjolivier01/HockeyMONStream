#pragma once

#include "cupano/pano/cudaMat.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"
#include "hstream/src/libs/draw_display/Fonts.h"
#include "hstream/src/libs/scoreboard/Scoreboard.h"

#include <string>
#include <vector>

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
  bool UsesRuntimeOutputSize() const override;
  absl::StatusOr<videoprep::RuntimeOutputSize> PrepareRuntimeOutputSize(
      NvDsBatchMeta* batch_meta,
      NvBufSurface* in_surface) override;

  gint AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) override;

 protected:
  absl::Status RenderDisplayMeta(surface::Surface surface, const NvDsFrameMeta* frame_meta, cudaStream_t stream);
  absl::Status RenderScoreboard(surface::Surface in_surface, surface::Surface out_surface, cudaStream_t stream);
  absl::Status EnsureScoreboardPerspectiveConfigured(surface::Surface stitched_surface);
  absl::Status LoadScoreboardPerspectiveFromConfig();
  void TransformObjectMetaForOutput(
      NvDsFrameMeta* frame_meta,
      float scale_w,
      float scale_h,
      const BBox& src_rect,
      float angle,
      const Point& anchor_point,
      const BBox& crop_box,
      const BBox& output_rect);

  absl::Mutex mu_process_;
  bool use_unfused_kernels_{false};
  bool show_{false};
  bool no_crop_{false};
  size_t frame_count_{0};
  std::shared_ptr<draw_display::FontCache> font_cache_;
  float render_scale_{0.5};
  std::unique_ptr<hm::CudaMat<uchar4>> display_surface_;
  float fixed_edge_rotation_angle_{10.0};
  bool show_scoreboard_{false};
  float scoreboard_width_ratio_{1.0 / 8};
  float scoreboard_height_ratio_{1.0 / 8};
  std::string scoreboard_projected_width_;
  std::string scoreboard_projected_height_;
  float scoreboard_scale_{1.0};
  std::unique_ptr<hm::scoreboard::Scoreboard<uchar4>> scoreboard_;
  std::vector<cv::Point2f> scoreboard_perspective_polygion_;
  bool scoreboard_disabled_{false};
  bool scoreboard_configure_attempted_{false};
  std::string config_file_;
  size_t scoreboard_warp_interval_{3};
  NvBufSurfaceParams display_dest_params_;
  bool plot_play_tracking_{false};
  bool plot_player_tracking_{false};
  bool transform_object_meta_{false};
  bool runtime_output_size_{false};
  size_t runtime_output_max_width_{0};
  size_t runtime_output_max_height_{0};
};

} // namespace playcropper
} // namespace hm
