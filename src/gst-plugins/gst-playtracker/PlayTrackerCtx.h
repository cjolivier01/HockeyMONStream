#pragma once

#include <cuda_runtime.h>
#include <nvdsmeta.h>

#include <atomic>
#include <list>
#include <memory>
#include <optional>
#include <string>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerRuntimeConfig.h"
#include "hstream/src/libs/common/ManagedObject.h"
// #include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"

#include "hockeymom/csrc/play_tracker/PlayTracker.h"
#include "yaml-cpp/yaml.h"

typedef struct GstDsPlayTrackerFrame GstDsPlayTrackerFrame;

namespace gst_hm_playtracker {
hm::play_tracker::PlayTrackerConfig create_play_tracker_config(const hm::BBox& arena_box, const YAML::Node& yaml);
}

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
  std::atomic<bool> draw{false};
  std::atomic<unsigned> preview_overlay_flags{0};
  std::atomic<bool> preview_snapshot_failure_reported{false};
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

absl::Status DsPlayTrackerCtxApplyRuntimeTuning(DsPlayTrackerCtx* ctx, const DsPlayTrackerRuntimeTuning& tuning);

// Controls production display-metadata generation without restarting tracking.
void DsPlayTrackerCtxSetDraw(DsPlayTrackerCtx* ctx, bool draw);

enum DsPlayTrackerPreviewOverlayFlags : unsigned {
  kPreviewOverlayPlayers = 1U << 0,
  kPreviewOverlayPlay = 1U << 1,
  kPreviewOverlayTransformRequired = 1U << 2,
  kPreviewOverlayAll = kPreviewOverlayPlayers | kPreviewOverlayPlay | kPreviewOverlayTransformRequired,
};

// Controls immutable preview-only metadata generation without changing the
// configured production draw path.
void DsPlayTrackerCtxSetPreviewOverlayFlags(DsPlayTrackerCtx* ctx, unsigned flags);
bool DsPlayTrackerAttachPreviewSnapshot(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame);

// Drops per-source position/velocity history while preserving accumulated live
// tuning. Used after a flushing playback seek before the next frame arrives.
void DsPlayTrackerCtxResetTracking(DsPlayTrackerCtx* ctx);

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
