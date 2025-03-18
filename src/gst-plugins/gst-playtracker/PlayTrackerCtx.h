#pragma once

#include <string>

#include <cuda_runtime.h>
#include <nvbufsurface.h>
#include <nvdsmeta.h>

#include "hockeymom/csrc/play_tracker/PlayTracker.h"

typedef struct DsPlayTrackerCtx DsPlayTrackerCtx;
typedef struct GstDsPlayTrackerFrame GstDsPlayTrackerFrame;

// Init parameters structure as input, required for instantiating
// playtracker_lib
struct DsPlayTrackerInitParams {
  std::string play_tracker_config_file;

  bool draw{false};
  // The class id we will set for the play box
  static constexpr inline int kPlayBoxClassIdBase = 99;
};

// Initialize library context
DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* init_params);

bool DsPlayTrackerProcessFrame(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame, cudaStream_t stream);

// Deinitialize library context
void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx);

struct GstDsPlayTrackerFrame {
  /** NvDsObjectParams belonging to the object to be classified. */
  NvDsObjectMeta* obj_meta = nullptr;
  NvDsFrameMeta* frame_meta = nullptr;
  /** Index of the frame in the batched input GstBuffer. Not required for
   * classifiers. */
  guint batch_index = 0;
  /** Frame number of the frame from the source. */
  gulong frame_num = 0;

  hm::play_tracker::PlayTrackerResults play_tracker_results;

  /** The buffer structure the object / frame was converted from. */
  NvBufSurfaceParams* input_surf_params = nullptr;
};
