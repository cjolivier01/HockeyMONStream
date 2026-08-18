#pragma once

#include "absl/status/statusor.h"
#include "hstream/src/libs/common/Status.h"

#include "cupano/cuda/cudaTypes.h"
#include "cupano/pano/cudaPano.h"

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"

#include <atomic>
#include <mutex>
#include <optional>
#include <set>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hm {
namespace stitcher {

struct RuntimeFrameKey {
  gint frame_num;
  guint source_id;
};

absl::StatusOr<std::pair<size_t, size_t>> select_runtime_stitch_pair(
    const std::vector<RuntimeFrameKey>& frames,
    const std::set<guint>& eos_source_ids = {},
    bool pipeline_eos_seen = false);

/**
 * Enforces the lossless stitched-frame contract. The first frame must be zero, every later frame must be exactly the
 * previous frame plus one, and the returned value is the last accepted frame number.
 */
absl::StatusOr<gint> validate_stitch_frame_continuity(
    const std::vector<gint>& frame_numbers,
    std::optional<gint> previous_frame_num = std::nullopt);

/** Resets output occupancy for a new stitched batch without changing the pool's allocation capacity. */
absl::Status prepare_stitch_output_surface(NvBufSurface* output_surface, size_t planned_frames);

struct OnePassCalibrationProgressPlan {
  bool report;
  bool create_mask;
  bool complete;
};

class OnePassCalibrationCompletionLatch {
 public:
  bool delivered() const;
  bool try_begin_delivery();
  void finish_delivery(bool delivered);

 private:
  // 0 = available, 1 = a stitcher is posting, 2 = delivered to the bus.
  std::atomic<unsigned> state_{0};
};

// Keeps resumed calibration observable even when stitch mappings were already
// committed before the prior run stopped. report_latched preserves a progress
// sequence that this run already announced while it creates the rink mask.
// process_completion_latched suppresses duplicate progress from stitchers
// recreated after another instance completed the shared game calibration.
OnePassCalibrationProgressPlan one_pass_calibration_progress_plan(
    bool configured_during_run,
    bool mask_configured,
    bool report_latched = false,
    bool process_completion_latched = false);

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
  guint GetOutputBatchSize(guint input_batch_size, guint configured_batch_size) const override;
  absl::StatusOr<videoprep::RuntimeOutputSize> PrepareRuntimeOutputSize(
      NvDsBatchMeta* batch_meta,
      NvBufSurface* in_surface) override;

 private:
  using STITCHER_FP32 = hm::pano::cuda::CudaStitchPano<uchar4, float4>;
  using STITCHER_FP16 = hm::pano::cuda::CudaStitchPano<uchar4, half3>;
  enum class StitchComputePrecision {
    kFp32,
    kFp16,
  };
  struct EosSnapshot {
    bool pipeline_eos_seen{false};
    std::set<guint> source_ids;
  };

  absl::Status ensure_stitcher();
  absl::Status reload_stitcher();
  absl::Status configure_one_pass_from_surfaces(
      hm::surface::Surface incoming_surface_left,
      hm::surface::Surface incoming_surface_right);
  absl::Status apply_post_stitch_rotation(
      hm::surface::Surface surface,
      size_t width,
      size_t height,
      double post_stitch_rotate_degrees);
  absl::Status ensure_rotation_scratch(const hm::surface::Surface& surface, size_t width, size_t height);
  void release_rotation_scratch();
  EosSnapshot snapshot_eos_for_buffer(GstBuffer* inbuf);
  EosSnapshot snapshot_eos_for_surface(NvBufSurface* in_surface);
  void update_canvas_hints(size_t width, size_t height) {
    canvas_width_hint_ = width;
    canvas_height_hint_ = height;
  }
  bool has_stitcher() const ABSL_EXCLUSIVE_LOCKS_REQUIRED(stitcher_mu_) {
    return stitcher_fp32_ || stitcher_fp16_;
  }

  absl::Mutex stitcher_mu_;
  std::unique_ptr<STITCHER_FP32> stitcher_fp32_ ABSL_GUARDED_BY(stitcher_mu_);
  std::unique_ptr<STITCHER_FP16> stitcher_fp16_ ABSL_GUARDED_BY(stitcher_mu_);
  std::string hugin_generation_id_ ABSL_GUARDED_BY(stitcher_mu_);
  std::string config_file_;
  std::string calibration_invalidation_id_;
  GstElement* owner_element_{nullptr};
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
  bool calibration_completion_reported_{false};
  bool calibration_completion_ready_{false};
  std::atomic_bool calibration_cancelled_{false};
  size_t left_frame_offset_ns_{0}, right_frame_offset_ns_{0};
  bool show_{false};
  bool match_exposure_{false};
  bool minimize_blend_{false};
  bool require_decoded_frame_sequence_meta_{false};
  StitchComputePrecision stitch_compute_precision_{StitchComputePrecision::kFp32};
  std::mutex eos_mu_;
  bool pipeline_eos_seen_{false};
  std::set<guint> eos_source_ids_;
  std::unordered_map<NvBufSurface*, EosSnapshot> eos_snapshot_by_surface_;
  std::optional<gint> last_stitched_frame_num_;
  std::atomic<double> post_stitch_rotate_degrees_{0.0};
  void* rotation_scratch_data_{nullptr};
  size_t rotation_scratch_pitch_{0};
  size_t rotation_scratch_width_{0};
  size_t rotation_scratch_height_{0};
  NvBufSurfaceParams rotation_scratch_params_{};
};

} // namespace stitcher
} // namespace hm
