/*
 * SPDX-FileCopyrightText: Copyright (c) 2017-2020 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include "dscameraman_lib.h"
#include <stdio.h>
#include <stdlib.h>

struct DsCameraManCtx {
  DsCameraManInitParams initParams;
};


namespace {
 // HACK: Duplciated from PlayTrackerCtx, nbeeds ot be part of config
static constexpr inline int kPlayBoxClassIdBase = 99;
}
DsCameraManCtx* DsCameraManCtxInit(DsCameraManInitParams* initParams) {
  DsCameraManCtx* ctx = (DsCameraManCtx*)calloc(1, sizeof(DsCameraManCtx));
  ctx->initParams = *initParams;
  return ctx;
}

// In case of an actual processing library, processing on data wil be completed
// in this function and output will be returned
DsCameraManOutput* DsCameraManProcess(GstDsCameraManFrame& frame, DsCameraManCtx* ctx, unsigned char* data) {
  DsCameraManOutput* out = (DsCameraManOutput*)calloc(1, sizeof(DsCameraManOutput));

  if (data != NULL) {
    // Process your data here
  }

  hm::BBox camera_box;
  for (NvDsMetaList* l_obj = frame.frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    if (obj_meta->class_id != kPlayBoxClassIdBase) {
      continue;
    }
    camera_box.left = obj_meta->rect_params.left;
    camera_box.top = obj_meta->rect_params.top;
    camera_box.right = obj_meta->rect_params.left + obj_meta->rect_params.width;
    camera_box.bottom = obj_meta->rect_params.top + obj_meta->rect_params.height;
    out->ops.final_camera_bbox = camera_box;
    break;
  }

  // Fill output structure using processed output
  // Here, we fake some detected objects and labels
#if 0
  if (ctx->initParams.fullFrame) {
    out->numObjects = 2;
    out->object[0] = (DsCameraManObject){
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        "Obj0"};

    out->object[1] = (DsCameraManObject){
        (float)(ctx->initParams.processingWidth) / 2,
        (float)(ctx->initParams.processingHeight) / 2,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        "Obj1"};
  } else {
    out->numObjects = 1;
    out->object[0] = (DsCameraManObject){
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        ""};
    // Set the object label
    snprintf(out->object[0].label, 64, "Obj_label");
  }
#endif
  return out;
}

void DsCameraManCtxDeinit(DsCameraManCtx* ctx) {
  free(ctx);
}
