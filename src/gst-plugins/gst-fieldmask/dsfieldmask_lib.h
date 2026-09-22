#pragma once

#include "hstream/src/libs/common/IceBoundary.h"
#include "nvbufsurface.h"
#include "nvdsmeta.h"

#include <string>

#include "absl/status/status.h"

#define MAX_LABEL_SIZE 128

typedef struct DsFieldMaskCtx DsFieldMaskCtx;

// Init parameters structure as input, required for instantiating dsfieldmask_lib
typedef struct {
  std::string detection_mask_file;
  float raise_bbox_center_by_height_ratio{0.0F};
  float lower_bbox_bottom_by_height_ratio{0.0F};
  float left_bbox_by_half_width_ratio{0.2F};
  float right_bbox_by_half_width_ratio{0.2F};
  hm::fieldmask::RinkMaskInsets mask_insets;
  bool require_existing_mask{false};
} DsFieldMaskInitParams;

// Initialize library context
DsFieldMaskCtx* DsFieldMaskCtxInit(DsFieldMaskInitParams* init_params);

// Called by the streaming thread with a locked snapshot of live properties.
void DsFieldMaskSetOffsets(DsFieldMaskCtx* ctx, const hm::fieldmask::IceBoundaryOffsets& offsets);
void DsFieldMaskSetInsets(DsFieldMaskCtx* ctx, const hm::fieldmask::RinkMaskInsets& insets);

absl::Status DsFieldMaskProcessFrame(
    NvBufSurface* surface,
    size_t frame_index,
    NvDsFrameMeta* frame_meta,
    DsFieldMaskCtx* ctx,
    bool draw);

// Deinitialize library context
void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx);
