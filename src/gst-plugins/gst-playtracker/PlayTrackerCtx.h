#pragma once

#include <cuda_runtime.h>
#include <nvdsmeta.h>

#include <list>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/libs/common/ManagedObject.h"
// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"

#include "hockeymom/csrc/play_tracker/PlayTracker.h"
#include "yaml-cpp/yaml.h"

typedef struct GstDsPlayTrackerFrame GstDsPlayTrackerFrame;

namespace gst_hm_playtracker {
hm::play_tracker::PlayTrackerConfig create_play_tracker_config(const hm::BBox& arena_box, const YAML::Node& yaml);
}

struct DsPlayTrackerRuntimeTuning {
  std::optional<int> stop_on_dir_change_delay;
  std::optional<bool> cancel_on_opposite;
  std::optional<int> cancel_hysteresis_frames;
  std::optional<int> stop_delay_cooldown_frames;
  std::optional<int> post_nonstop_stop_delay_count;
  std::optional<int> time_to_dest_speed_limit_frames;
  std::optional<int> overshoot_stop_delay_count;
  std::optional<float> overshoot_scale_speed_ratio;
  std::optional<float> max_speed_x;
  std::optional<float> max_speed_y;
  std::optional<float> max_accel_x;
  std::optional<float> max_accel_y;
  bool apply_to_fast_box{false};
  bool apply_to_follower_box{true};
  bool update_motion_tuning{true};
  bool update_camera_geometry{false};
  float arena_angle_from_vertical{0.0f};
  float dynamic_acceleration_scaling{1.0f};
};

// Init parameters structure as input, required for instantiating
// playtracker_lib
struct DsPlayTrackerInitParams {
  // Stuff we own, even if we don;t know (or care) what it is
  std::vector<std::shared_ptr<hm::ManagedObject>> owned_objects;
  std::string play_tracker_config_file;
  bool draw{false};
  // // The class id we will set for the play box
  static constexpr inline int kPlayBoxClassIdBase = 99;
};

struct DsPlayTrackerCtx {
  DsPlayTrackerInitParams initParams;
  std::optional<DsPlayTrackerRuntimeTuning> detector_runtime_tuning;
  std::optional<DsPlayTrackerRuntimeTuning> fast_box_runtime_tuning;
  std::optional<DsPlayTrackerRuntimeTuning> follower_box_runtime_tuning;
  struct PlayTracker {
    hm::play_tracker::PlayTrackerConfig base_play_tracker_config;
    hm::play_tracker::PlayTrackerConfig play_tracker_config;
    std::unique_ptr<hm::play_tracker::PlayTracker> play_tracker;
    bool has_received_tracks{false};
  };
  // source_id -> play_tracker
  std::unordered_map<size_t, PlayTracker> play_trackers;
  hm::BBox arena_box;
};

// Initialize library context
DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* init_params);

absl::Status DsPlayTrackerValidateConfigFile(const std::string& config_file);

absl::StatusOr<DsPlayTrackerRuntimeTuning> DsPlayTrackerLoadRuntimeTuning(const std::string& config_file);

absl::Status DsPlayTrackerCtxApplyRuntimeTuning(DsPlayTrackerCtx* ctx, const DsPlayTrackerRuntimeTuning& tuning);

bool DsPlayTrackerProcessFrame(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame, cudaStream_t stream);

absl::Status DsPlayTrackerDrawToDisplayMeta(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame);

// Deinitialize library context
void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx);

void DsPlayTrackerAttachMetadataFullFrame(
    NvDsFrameMeta* frame_meta,
    const hm::play_tracker::PlayTrackerResults& play_results);

struct GstDsPlayTrackerFrame {
  /** NvDsObjectParams belonging to the object to be classified. */
  // NvDsObjectMeta* obj_meta = nullptr;
  NvDsFrameMeta* frame_meta = nullptr;
  /** Index of the frame in the batched input GstBuffer. Not required for
   * classifiers. */
  guint batch_index = 0;

  hm::play_tracker::PlayTrackerResults play_tracker_results;

  /** The buffer structure the object / frame was converted from. */
  NvBufSurfaceParams* input_surf_params = nullptr;
};
