#pragma once

#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"

#include <mutex>
#include <vector>
#include "absl/status/status.h"

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
  bool HandleEvent(GstEvent* event) override;
  bool SetProperty(const Property& prop) override;
  // DSCustomLibraryBase-

  absl::Status GenerateOutput(NvDsBatchMeta* batch_meta, NvBufSurface* in_surface, NvBufSurface* out_surface) override;

  // Test/diagnostic accessors for proving runtime tuning preserves the active
  // context and tracker objects.
  DsPlayTrackerCtx* contextForTesting() const {
    return pt_context_;
  }

 protected:
  absl::Status ReloadContextFromConfig();

  DsPlayTrackerInitParams init_params_;
  std::string play_tracker_config_source_file_;
  std::mutex context_mu_;
  DsPlayTrackerCtx* pt_context_{nullptr};
  std::vector<DsPlayTrackerRuntimeTuning> runtime_tuning_history_;
  hm::play_tracker::PlayTrackerResults prev_play_tracker_results_;
  float fixed_edge_rotation_angle_left_{10.0};
  float fixed_edge_rotation_angle_right_{10.0};
  // Dynamic acceleration scaling (usually to slower) on the last live-box only
  float dynamic_acceleration_scaling_{1.0};
  size_t frame_counter_{0};
  size_t frame_calculation_interval_{1};
  bool show_{false};
};

} // namespace playtracker
} // namespace hm
