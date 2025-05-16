#pragma once

#include "nvdsmeta.h"
//#include "deepstream/sources/includes/nvbufsurface.h"
#include "nvbufsurface.h"
#include "src/gst-plugins/gst-fieldmask/fieldmask_payload.h"

#include <string>

#include "absl/status/status.h"

#define MAX_LABEL_SIZE 128

typedef struct DsFieldMaskCtx DsFieldMaskCtx;

// Init parameters structure as input, required for instantiating dsfieldmask_lib
typedef struct {
  std::string detection_mask_file;
} DsFieldMaskInitParams;

// Initialize library context
DsFieldMaskCtx* DsFieldMaskCtxInit(DsFieldMaskInitParams* init_params);

absl::Status DsFieldMaskProcessFrame(NvBufSurface* surface, size_t frame_index, NvDsFrameMeta* frame_meta, DsFieldMaskCtx* ctx);

// Deinitialize library context
void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx);
