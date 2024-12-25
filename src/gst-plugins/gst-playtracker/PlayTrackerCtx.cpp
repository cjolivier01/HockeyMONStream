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
#include "hockeymom/csrc/play_tracker/PlayTracker.h"
#include "libs/common/ConfigYaml.h"

#include "gstplaytracker.h"
#include "kmeans.h"

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

using namespace hm::play_tracker;

PlayDetectorConfig create_play_detector_config(const YAML::Node& yaml) {
  PlayDetectorConfig config;
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, max_positions);
  SET_LOCATOR(locator, config, max_velocity_positions);
  SET_LOCATOR(locator, config, frame_step);
  SET_LOCATOR(locator, config, fps_speed_scale);
  SET_LOCATOR(locator, config, min_considered_group_velocity);
  SET_LOCATOR(locator, config, group_ratio_threshold);
  SET_LOCATOR(locator, config, group_velocity_speed_ratio);
  SET_LOCATOR(locator, config, scale_speed_constraints);
  SET_LOCATOR(locator, config, nonstop_delay_count);
  SET_LOCATOR(locator, config, overshoot_scale_speed_ratio);
  set_config_from_yaml(yaml, locator);
  return config;
}

ResizingConfig create_resizing_config(const YAML::Node& yaml) {
  ResizingConfig config;
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, resizing_enabled);
  SET_LOCATOR(locator, config, max_speed_w);
  SET_LOCATOR(locator, config, max_speed_h);
  SET_LOCATOR(locator, config, max_accel_w);
  SET_LOCATOR(locator, config, max_accel_h);
  SET_LOCATOR(locator, config, min_width);
  SET_LOCATOR(locator, config, min_height);
  SET_LOCATOR(locator, config, max_width);
  SET_LOCATOR(locator, config, max_height);
  SET_LOCATOR(locator, config, stop_on_dir_change);
  SET_LOCATOR(locator, config, sticky_sizing);
  SET_LOCATOR(locator, config, size_ratio_thresh_grow_dw);
  SET_LOCATOR(locator, config, size_ratio_thresh_grow_dh);
  SET_LOCATOR(locator, config, size_ratio_thresh_shrink_dw);
  SET_LOCATOR(locator, config, size_ratio_thresh_shrink_dh);
  set_config_from_yaml(yaml, locator);
  return config;
}
TranslatingBoxConfig create_translating_box_config(const BBox& arena_box, const YAML::Node& yaml) {
  TranslatingBoxConfig config;
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, translation_enabled);
  SET_LOCATOR(locator, config, max_speed_x);
  SET_LOCATOR(locator, config, max_speed_y);
  SET_LOCATOR(locator, config, max_accel_x);
  SET_LOCATOR(locator, config, max_accel_y);
  SET_LOCATOR(locator, config, stop_on_dir_change);
  SET_LOCATOR(locator, config, sticky_translation);
  SET_LOCATOR(locator, config, sticky_size_ratio_to_frame_width);
  SET_LOCATOR(locator, config, sticky_translation_gaussian_mult);
  SET_LOCATOR(locator, config, unsticky_translation_size_ratio);
  config.arena_box = arena_box;
  set_config_from_yaml(yaml, locator);
  return config;
}

LivingBoxConfig create_living_box_config(
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  LivingBoxConfig config;
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, scale_dest_width);
  SET_LOCATOR(locator, config, scale_dest_height);
  SET_LOCATOR(locator, config, clamp_scaled_input_box);
  config.fixed_aspect_ratio = fixed_aspect_ratio;
  set_config_from_yaml(yaml, locator);
  return config;
}

AllLivingBoxConfig create_all_living_box_config(
    const BBox& arena_box,
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  AllLivingBoxConfig config;
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, name);
  *((ResizingConfig*)&config) = create_resizing_config(yaml);
  *((TranslatingBoxConfig*)&config) = create_translating_box_config(arena_box, yaml);
  *((LivingBoxConfig*)&config) = create_living_box_config(yaml, fixed_aspect_ratio);
  return config;
}

PlayTrackerConfig create_play_tracker_config(const BBox& arena_box, const YAML::Node& yaml) {
  PlayTrackerConfig config;
  hm::utils::ConfigLocator locator;

  if (yaml["live-boxes"]) {
    YAML::Node live_boxes = yaml["live-boxes"];
    // Iterate over the list
    for (const auto& box_yaml : live_boxes) {
      config.living_boxes.emplace_back(create_all_living_box_config(arena_box, box_yaml));
    }
  }
  config.play_detector = create_play_detector_config(yaml);

  SET_LOCATOR(locator, config, no_wide_start);
  SET_LOCATOR(locator, config, max_lost_track_age);
  SET_LOCATOR(locator, config, ignore_largest_bbox);
  set_config_from_yaml(yaml, locator);
  return config;
}
} // namespace gst_hm

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
  // const auto kmeans_type = hm::kmeans::KMEANS_TYPE::KM_OMP;
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
