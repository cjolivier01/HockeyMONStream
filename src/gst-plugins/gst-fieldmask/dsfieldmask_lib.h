#pragma once

#include "nvdsmeta.h"

#include <string>

#define MAX_LABEL_SIZE 128

typedef struct DsFieldMaskCtx DsFieldMaskCtx;

// Init parameters structure as input, required for instantiating dsfieldmask_lib
typedef struct {
  // Width at which frame/object will be scaled
  int processingWidth;
  // height at which frame/object will be scaled
  int processingHeight;
  // Flag to indicate whether operating on crops of full frame
  int fullFrame;
  std::string detection_mask_file;
} DsFieldMaskInitParams;

// Detected/Labelled object structure, stores bounding box info along with label
typedef struct {
  float left;
  float top;
  float width;
  float height;
  char label[MAX_LABEL_SIZE];
} DsFieldMaskObject;

// Output data returned after processing
typedef struct {
  int numObjects;
  DsFieldMaskObject object[4];
} DsFieldMaskOutput;

// Initialize library context
DsFieldMaskCtx* DsFieldMaskCtxInit(DsFieldMaskInitParams* init_params);

void DsFieldMaskProcessFrame(NvDsFrameMeta* frame_meta, DsFieldMaskCtx* ctx);

// Deinitialize library context
void DsFieldMaskCtxDeinit(DsFieldMaskCtx* ctx);

