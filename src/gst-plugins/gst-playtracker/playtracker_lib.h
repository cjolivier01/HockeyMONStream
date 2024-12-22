#pragma once

#include "nvdsmeta.h"

#include <string>

#define MAX_LABEL_SIZE 128

typedef struct DsPlayTrackerCtx DsPlayTrackerCtx;

// Init parameters structure as input, required for instantiating playtracker_lib
typedef struct {
  // Width at which frame/object will be scaled
  int processingWidth;
  // height at which frame/object will be scaled
  int processingHeight;
  // Flag to indicate whether operating on crops of full frame
  int fullFrame;
  std::string detection_mask_file;
} DsPlayTrackerInitParams;

// Detected/Labelled object structure, stores bounding box info along with label
typedef struct {
  float left;
  float top;
  float width;
  float height;
  char label[MAX_LABEL_SIZE];
} DsPlayTrackerObject;

// Output data returned after processing
typedef struct {
  int numObjects;
  DsPlayTrackerObject object[4];
} DsPlayTrackerOutput;

// Initialize library context
DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* init_params);

void DsPlayTrackerProcessFrame(NvDsFrameMeta* frame_meta, DsPlayTrackerCtx* ctx);

// Dequeue processed output
DsPlayTrackerOutput* DsPlayTrackerProcess(DsPlayTrackerCtx* ctx, unsigned char* data);

// Deinitialize library context
void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx);

