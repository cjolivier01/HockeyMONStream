#pragma once

#include "hockeymom/csrc/play_tracker/BoxUtils.h"

#include "gstdscameraman_optimized.h"

#include "nvdsmeta.h"

#define MAX_LABEL_SIZE 128
// #ifdef __cplusplus
// extern "C" {
// #endif

typedef struct DsCameraManCtx DsCameraManCtx;

// Init parameters structure as input, required for instantiating dscameraman_lib
typedef struct {
  // Width at which frame/object will be scaled
  int processingWidth;
  // height at which frame/object will be scaled
  int processingHeight;
  // Flag to indicate whether operating on crops of full frame
  int fullFrame;
} DsCameraManInitParams;

// Detected/Labelled object structure, stores bounding box info along with label
typedef struct {
  float left;
  float top;
  float width;
  float height;
  char label[MAX_LABEL_SIZE];
} DsCameraManObject;

struct DsCameraManOperations {
  hm::BBox final_camera_bbox;
};

// Output data returned after processing
typedef struct {
  int numObjects;
  DsCameraManObject object[4];
  DsCameraManOperations ops;
} DsCameraManOutput;

// Initialize library context
DsCameraManCtx* DsCameraManCtxInit(DsCameraManInitParams* init_params);

struct GstDsCameraManFrame;

// Dequeue processed output
DsCameraManOutput* DsCameraManProcess(NvDsFrameMeta* frame_meta, DsCameraManCtx* ctx);

// Deinitialize library context
void DsCameraManCtxDeinit(DsCameraManCtx* ctx);

// #ifdef __cplusplus
// }
// #endif
