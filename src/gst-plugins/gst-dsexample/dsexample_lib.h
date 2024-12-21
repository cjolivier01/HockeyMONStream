#pragma once

#include "nvdsmeta.h"

#include <string>

#define MAX_LABEL_SIZE 128

typedef struct DsExampleCtx DsExampleCtx;

// Init parameters structure as input, required for instantiating dsexample_lib
typedef struct {
  // Width at which frame/object will be scaled
  int processingWidth;
  // height at which frame/object will be scaled
  int processingHeight;
  // Flag to indicate whether operating on crops of full frame
  int fullFrame;
  std::string detection_mask_file;
} DsExampleInitParams;

// Detected/Labelled object structure, stores bounding box info along with label
typedef struct {
  float left;
  float top;
  float width;
  float height;
  char label[MAX_LABEL_SIZE];
} DsExampleObject;

// Output data returned after processing
typedef struct {
  int numObjects;
  DsExampleObject object[4];
} DsExampleOutput;

// Initialize library context
DsExampleCtx* DsExampleCtxInit(DsExampleInitParams* init_params);

void DsExampleProcessFrame(NvDsFrameMeta* frame_meta, DsExampleCtx* ctx);

// Dequeue processed output
DsExampleOutput* DsExampleProcess(DsExampleCtx* ctx, unsigned char* data);

// Deinitialize library context
void DsExampleCtxDeinit(DsExampleCtx* ctx);

