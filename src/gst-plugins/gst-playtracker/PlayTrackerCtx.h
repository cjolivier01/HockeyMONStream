#pragma once

#include <cuda_runtime.h>
#include <nvdsmeta.h>

#include <list>
#include <memory>
#include <string>

#include "absl/status/status.h"
#include "hstream/src/libs/common/ManagedObject.h"
//#include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"

#include "hockeymom/csrc/play_tracker/PlayTracker.h"

typedef struct GstDsPlayTrackerFrame GstDsPlayTrackerFrame;

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
  struct PlayTracker {
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

bool DsPlayTrackerProcessFrame(DsPlayTrackerCtx* ctx,
                               GstDsPlayTrackerFrame& frame,
                               cudaStream_t stream);

absl::Status DsPlayTrackerDrawToDisplayMeta(DsPlayTrackerCtx* ctx,
                                            GstDsPlayTrackerFrame& frame);

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
