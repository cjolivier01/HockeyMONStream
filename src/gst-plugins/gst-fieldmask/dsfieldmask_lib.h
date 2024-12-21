#pragma once

#include "nvdsmeta.h"

#include <string>

#define MAX_LABEL_SIZE 128

typedef struct DsFieldMaskCtx DsFieldMaskCtx;

// Init parameters structure as input, required for instantiating dsfieldmask_lib
typedef struct {
  std::string detection_mask_file;
} DsFieldMaskInitParams;

// Initialize library context
DsFieldMaskCtx* DsFieldMaskCtxInit(DsFieldMaskInitParams* init_params);

void DsFieldMaskProcessFrame(NvDsFrameMeta* frame_meta, DsFieldMaskCtx* ctx);

// Deinitialize library context
void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx);

