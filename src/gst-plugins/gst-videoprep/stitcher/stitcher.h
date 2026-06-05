#pragma once

#include "absl/status/statusor.h"
#include "hstream/src/libs/common/Status.h"

#include "cupano/pano/cudaPano.h"

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"

#include <mutex>
#include <set>
#include <unordered_map>

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

  bool HandleEvent(GstEvent* event) override;
  void Shutdown() override;

  char* QueryProperties() override {
    assert(false);
    return strdup("");
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override;

  // DSCustomLibraryBase-

  absl::Status GenerateOutput(NvDsBatchMeta* batch_meta, NvBufSurface* in_surface, NvBufSurface* out_surface) override;
  bool UsesRuntimeOutputSize() const override;
  absl::StatusOr<videoprep::RuntimeOutputSize> PrepareRuntimeOutputSize(
      NvDsBatchMeta* batch_meta,
      NvBufSurface* in_surface) override;

 private:
  using STITCHER = hm::pano::cuda::CudaStitchPano<uchar4, float4>;
  struct EosSnapshot {
    bool pipeline_eos_seen{false};
    std::set<guint> source_ids;
  };

  absl::StatusOr<STITCHER*> get_stitcher();
  absl::Status reload_stitcher();
  absl::Status configure_one_pass_from_surfaces(
      hm::surface::Surface incoming_surface_left,
      hm::surface::Surface incoming_surface_right);
  absl::Status apply_post_stitch_rotation(hm::surface::Surface surface, size_t width, size_t height);
  absl::Status ensure_rotation_scratch(const hm::surface::Surface& surface, size_t width, size_t height);
  void release_rotation_scratch();
  EosSnapshot snapshot_eos_for_buffer(GstBuffer* inbuf);
  EosSnapshot snapshot_eos_for_surface(NvBufSurface* in_surface);
  void update_canvas_hints(size_t width, size_t height) {
    canvas_width_hint_ = width;
    canvas_height_hint_ = height;
  }

  absl::Mutex stitcher_mu_;
  std::unique_ptr<STITCHER> stitcher_ ABSL_GUARDED_BY(stitcher_mu_);
  std::string config_file_;
  std::mutex process_mu_;
  size_t process_pass_{0};
  bool configure_only_{false};
  bool one_pass_mode_{false};
  bool force_scoreboard_config_{false};
  size_t canvas_width_hint_{0};
  size_t canvas_height_hint_{0};
  bool configured_during_run_{false};
  bool logged_missing_masks_{false};
  bool orientation_ran_{false};
  bool field_mask_attempted_{false};
  size_t left_frame_offset_ns_{0}, right_frame_offset_ns_{0};
  bool show_{false};
  bool match_exposure_{false};
  bool minimize_blend_{false};
  std::mutex eos_mu_;
  bool pipeline_eos_seen_{false};
  std::set<guint> eos_source_ids_;
  std::unordered_map<NvBufSurface*, EosSnapshot> eos_snapshot_by_surface_;
  double post_stitch_rotate_degrees_{0.0};
  void* rotation_scratch_data_{nullptr};
  size_t rotation_scratch_pitch_{0};
  size_t rotation_scratch_width_{0};
  size_t rotation_scratch_height_{0};
  NvBufSurfaceParams rotation_scratch_params_{};
};

} // namespace stitcher
} // namespace hm
