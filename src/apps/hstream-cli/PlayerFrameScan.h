#pragma once

#include <gst/gst.h>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include "PlayerFrameScanMask.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/stitching/PlayerFrameOverlap.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

struct _AppCtx;
typedef struct _AppCtx AppCtx;
struct _NvDsFrameMeta;
typedef struct _NvDsFrameMeta NvDsFrameMeta;

namespace hm::pipeline {

class PlayerFrameScan {
 public:
  static absl::StatusOr<std::unique_ptr<PlayerFrameScan>> Create(
      AppCtx* app,
      const std::filesystem::path& game_directory,
      const stitching::PlayerFrameSelectionSettings& settings,
      uint64_t decode_anchor_ns);
  ~PlayerFrameScan();
  absl::Status Attach();
  // Called only after successful, quiescent pipeline shutdown.
  absl::Status Finish(const std::filesystem::path& report_path);

 private:
  PlayerFrameScan(AppCtx* app, std::filesystem::path game_directory, stitching::PlayerFrameSelectionSettings settings);
  static GstPadProbeReturn Gate(GstPad* pad, GstPadProbeInfo* info, gpointer data);
  static GstPadProbeReturn Observe(GstPad* pad, GstPadProbeInfo* info, gpointer data);
  absl::StatusOr<bool> GateFrame(GstBuffer* buffer);
  absl::Status ObserveFrame(GstBuffer* buffer);
  GstPadProbeReturn Fail(GstPad* pad, const absl::Status& status);

  AppCtx* app_;
  std::filesystem::path game_directory_;
  stitching::PlayerFrameSelectionSettings settings_;
  stitching::PlayerFrameSelectionContext context_;
  std::vector<stitching::PlayerFrameObservation> observations_;
  std::vector<stitching::PlayerFrameSourceBinding> sources_;
  std::optional<stitching::PlayerFrameOverlap> overlap_;
  std::optional<uint64_t> first_pts_;
  std::optional<uint64_t> last_sample_pts_;
  bool boundary_sample_sent_{false};
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
