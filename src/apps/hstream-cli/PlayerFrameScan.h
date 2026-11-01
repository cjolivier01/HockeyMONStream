#pragma once

#include <gst/gst.h>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "PlayerFrameScanMask.h"
#include "PlayerFrameScanTiming.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/PlayerFrameOverlap.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

struct _AppCtx;
typedef struct _AppCtx AppCtx;
struct _NvDsFrameMeta;
typedef struct _NvDsFrameMeta NvDsFrameMeta;

namespace hm::pipeline {

struct PlayerFrameScanSession {
  stitching::PlayerFrameScanWindow window;
  bool second_pass{false};
  stitching::PlayerFrameSelectionContext context;
  std::vector<stitching::PlayerFrameObservation> observations;
  std::vector<stitching::PlayerFrameSourceBinding> sources;
  uint64_t elapsed_before_pass() const {
    return second_pass ? window.first_duration_ns : 0;
  }
  uint64_t pass_duration() const {
    return second_pass ? window.second_duration_ns : window.first_duration_ns;
  }
};

class PlayerFrameScan {
 public:
  static absl::StatusOr<std::unique_ptr<PlayerFrameScan>> Create(
      AppCtx* app,
      const std::filesystem::path& game_directory,
      const stitching::PlayerFrameSelectionSettings& settings,
      uint64_t decode_anchor_ns,
      std::shared_ptr<PlayerFrameScanSession> session = {});
  ~PlayerFrameScan();
  absl::Status Attach();
  PlayerFrameScanProgress progress() const {
    auto result = timing_.progress();
    if (result.elapsed_ns && session_)
      *result.elapsed_ns += session_->elapsed_before_pass();
    return result;
  }
  // Called only after successful, quiescent pipeline shutdown.
  absl::Status Finish(const std::filesystem::path& report_path, bool continue_after_eos = false);

 private:
  PlayerFrameScan(
      AppCtx* app,
      std::filesystem::path game_directory,
      stitching::PlayerFrameSelectionSettings settings,
      std::shared_ptr<PlayerFrameScanSession> session);
  static GstPadProbeReturn Gate(GstPad* pad, GstPadProbeInfo* info, gpointer data);
  static GstPadProbeReturn Observe(GstPad* pad, GstPadProbeInfo* info, gpointer data);
  absl::StatusOr<bool> GateFrame(GstBuffer* buffer);
  absl::Status ObserveFrame(GstBuffer* buffer);
  GstPadProbeReturn Fail(GstPad* pad, const absl::Status& status);

  AppCtx* app_;
  std::filesystem::path game_directory_;
  stitching::PlayerFrameSelectionSettings settings_;
  std::shared_ptr<PlayerFrameScanSession> session_;
  PlayerFrameScanTiming timing_;
  stitching::PlayerFrameSelectionContext context_;
  std::vector<stitching::PlayerFrameObservation> observations_;
  std::vector<stitching::PlayerFrameSourceBinding> sources_;
  std::optional<stitching::PlayerFrameOverlap> overlap_;
  uint64_t decode_anchor_ns_{0};
  size_t max_output_width_{0};
  double rotation_degrees_{0};
  std::string initial_generation_;
  std::string expected_output_generation_;
  std::string expected_mask_fingerprint_;
  std::string detector_identity_;
  std::filesystem::path runtime_engine_path_;
  std::string runtime_detector_identity_;
  std::vector<std::pair<GstPad*, gulong>> probes_;
  std::mutex mutex_;
  absl::Status failure_;
};

} // namespace hm::pipeline
