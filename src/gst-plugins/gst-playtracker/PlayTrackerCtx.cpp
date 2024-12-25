/*
 * SPDX-FileCopyrightText: Copyright (c) 2017-2020 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier:
 * LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

#include "PlayTracker.h"
#include "gstplaytracker.h"
#include "kmeans.h"
//#include "kmeans_cuda_simple.h"
//#include "libs/k-means/headers/kmcuda_adapter.hpp"
//#include "utils.h"

#include <opencv2/opencv.hpp>

#include <cassert>
#include <vector>

#include <opencv4/opencv2/core/types.hpp>
#include <stdio.h>
#include <stdlib.h>

struct DsPlayTrackerCtx {
  DsPlayTrackerInitParams initParams;
};

namespace {} // namespace

namespace gst_hm {


}

DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* initParams) {
  DsPlayTrackerCtx* ctx = new DsPlayTrackerCtx();
  ctx->initParams = *initParams;
  return ctx;
}

void DsPlayTrackerProcessFrame(GstDsPlayTrackerFrame& frame, DsPlayTrackerCtx* ctx) {
  if (!frame.frame_meta->bInferDone) {
    return;
  }
  std::vector<float> points;
  const std::size_t object_count = g_list_length(frame.frame_meta->obj_meta_list);
  points.reserve(object_count * 2);
  for (NvDsMetaList* l_obj = frame.frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    const NvDsComp_BboxInfo& trackler_bbox_info = obj_meta->tracker_bbox_info;
    float x = trackler_bbox_info.org_bbox_coords.left + trackler_bbox_info.org_bbox_coords.width / 2;
    float y = trackler_bbox_info.org_bbox_coords.top + trackler_bbox_info.org_bbox_coords.height / 2;
    points.emplace_back(x);
    points.emplace_back(y);
  }
#if 1
  std::vector<int> assignments_2, assignments_3;
  const auto kmeans_type = hm::kmeans::KMEANS_TYPE::KM_SEQ;
  //const auto kmeans_type = hm::kmeans::KMEANS_TYPE::KM_OMP;
  if (object_count > 3) {
    hm::kmeans::compute_kmeans(
        points,
        /*numClusters=*/2,
        /*dim=*/2,
        /*numIterations=*/4,
        assignments_2,
        kmeans_type);
    // hm::cuda::kmeansCuda(points, /*numClusters=*/2, /*dim=*/2, /*numIterations=*/4, assignments_2);
  }
  if (object_count > 4) {
    hm::kmeans::compute_kmeans(
        points,
        /*numClusters=*/3,
        /*dim=*/2,
        /*numIterations=*/4,
        assignments_2,
        kmeans_type);
    // hm::cuda::kmeansCuda(points, /*numClusters=*/2, /*dim=*/2, /*numIterations=*/4, assignments_3);
  }
#endif
}

// In case of an actual processing library, processing on data wil be completed
// in this function and output will be returned
DsPlayTrackerOutput* DsPlayTrackerProcess(DsPlayTrackerCtx* ctx, unsigned char* data) {
  DsPlayTrackerOutput* out = (DsPlayTrackerOutput*)calloc(1, sizeof(DsPlayTrackerOutput));

  if (data != NULL) {
    // Process your data here
  }
  // Fill output structure using processed output
  // Here, we fake some detected objects and labels
  if (ctx->initParams.fullFrame) {
    out->numObjects = 2;
    out->object[0] = (DsPlayTrackerObject){
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        "Obj0"};

    out->object[1] = (DsPlayTrackerObject){
        (float)(ctx->initParams.processingWidth) / 2,
        (float)(ctx->initParams.processingHeight) / 2,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        "Obj1"};
  } else {
    out->numObjects = 1;
    out->object[0] = (DsPlayTrackerObject){
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        (float)(ctx->initParams.processingWidth) / 8,
        (float)(ctx->initParams.processingHeight) / 8,
        ""};
    // Set the object label
    snprintf(out->object[0].label, 64, "Obj_label");
  }

  return out;
}

void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx) {
  if (ctx) {
    delete ctx;
  }
}
