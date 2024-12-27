#pragma once

#include "nvdsmeta.h"

#include <string>

#define MAX_LABEL_SIZE 128

typedef struct DsReplaceFrameCtx DsReplaceFrameCtx;

// Init parameters structure as input, required for instantiating dsreplaceframe_lib
typedef struct {
  std::string detection_mask_file;
} DsReplaceFrameInitParams;

// Initialize library context
DsReplaceFrameCtx* DsReplaceFrameCtxInit(DsReplaceFrameInitParams* init_params);

void DsReplaceFrameProcessFrame(NvDsFrameMeta* frame_meta, DsReplaceFrameCtx* ctx);

// Deinitialize library context
void DsReplaceFrameCtxDeinit(DsReplaceFrameCtx* ctx);

