#pragma once

//#include "nvdsmeta.h"

#include <string>

#define MAX_LABEL_SIZE 128

typedef struct DsPlayTrackerCtx DsPlayTrackerCtx;
typedef struct GstDsPlayTrackerFrame GstDsPlayTrackerFrame;

// Init parameters structure as input, required for instantiating playtracker_lib
struct DsPlayTrackerInitParams {
  std::string play_tracker_config_file;

  bool draw{false};
  // The class id we will set for the play box
  static constexpr inline int kPlayBoxClassIdBase = 99;
};

// Initialize library context
DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* init_params);

bool DsPlayTrackerProcessFrame(GstDsPlayTrackerFrame& frame, DsPlayTrackerCtx* ctx);

// Deinitialize library context
void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx);

namespace gst_hm {

class GstPlayTracker {
  public:
    GstPlayTracker();
};

}
