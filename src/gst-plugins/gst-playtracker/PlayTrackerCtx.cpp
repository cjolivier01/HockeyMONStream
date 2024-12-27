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
#include "hockeymom/csrc/play_tracker/LivingBoxImpl.h"
#include "hockeymom/csrc/play_tracker/PlayTracker.h"
#include "libs/common/ConfigYaml.h"
#include "libs/common/PlotContext.h"

#include "gstplaytracker.h"

#include <nvdsmeta.h>

#include <cassert>
#include <vector>

#include <stdio.h>
#include <stdlib.h>

struct DsPlayTrackerCtx {
  DsPlayTrackerInitParams initParams;
  hm::play_tracker::PlayTrackerConfig play_tracker_config;
  std::optional<std::unique_ptr<hm::play_tracker::PlayTracker>> play_tracker;
};

namespace {} // namespace

namespace gst_hm {

using namespace hm;
using namespace hm::play_tracker;

PlayDetectorConfig create_play_detector_config(const YAML::Node& yaml, hm::utils::ConfigLocator& locator) {
  PlayDetectorConfig config;
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
  return config;
}

ResizingConfig create_resizing_config(const YAML::Node& yaml, hm::utils::ConfigLocator& locator) {
  ResizingConfig config;
  SET_LOCATOR(locator, config, resizing_enabled);
  SET_LOCATOR(locator, config, max_speed_w);
  SET_LOCATOR(locator, config, max_speed_h);
  SET_LOCATOR(locator, config, max_accel_w);
  SET_LOCATOR(locator, config, max_accel_h);
  SET_LOCATOR(locator, config, min_width);
  SET_LOCATOR(locator, config, min_height);
  SET_LOCATOR(locator, config, max_width);
  SET_LOCATOR(locator, config, max_height);
  SET_LOCATOR(locator, config, stop_resizing_on_dir_change);
  SET_LOCATOR(locator, config, sticky_sizing);
  SET_LOCATOR(locator, config, size_ratio_thresh_grow_dw);
  SET_LOCATOR(locator, config, size_ratio_thresh_grow_dh);
  SET_LOCATOR(locator, config, size_ratio_thresh_shrink_dw);
  SET_LOCATOR(locator, config, size_ratio_thresh_shrink_dh);
  return config;
}
TranslatingBoxConfig create_translating_box_config(
    const BBox& arena_box,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
  TranslatingBoxConfig config;
  SET_LOCATOR(locator, config, translation_enabled);
  SET_LOCATOR(locator, config, max_speed_x);
  SET_LOCATOR(locator, config, max_speed_y);
  SET_LOCATOR(locator, config, max_accel_x);
  SET_LOCATOR(locator, config, max_accel_y);
  SET_LOCATOR(locator, config, stop_translation_on_dir_change);
  SET_LOCATOR(locator, config, sticky_translation);
  SET_LOCATOR(locator, config, sticky_size_ratio_to_frame_width);
  SET_LOCATOR(locator, config, sticky_translation_gaussian_mult);
  SET_LOCATOR(locator, config, unsticky_translation_size_ratio);
  config.arena_box = arena_box;
  return config;
}

LivingBoxConfig create_living_box_config(
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  LivingBoxConfig config;
  SET_LOCATOR(locator, config, scale_dest_width);
  SET_LOCATOR(locator, config, scale_dest_height);
  SET_LOCATOR(locator, config, clamp_scaled_input_box);
  config.fixed_aspect_ratio = fixed_aspect_ratio;
  return config;
}

AllLivingBoxConfig create_all_living_box_config(
    const BBox& arena_box,
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  AllLivingBoxConfig config;
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, name);
  *((ResizingConfig*)&config) = create_resizing_config(yaml, locator);
  *((TranslatingBoxConfig*)&config) = create_translating_box_config(arena_box, yaml, locator);
  *((LivingBoxConfig*)&config) = create_living_box_config(yaml, locator, fixed_aspect_ratio);
  set_config_from_yaml(yaml, locator);
  return config;
}

const std::unordered_map<std::string, float> CAMERA_TYPE_MAX_SPEEDS = {
    {"GoPro", 200.0},
    {"Zhiwei", 200.0},
    {"LiveBarn", 150.0},
};

void adjust_config(const BBox& arena_box, PlayTrackerConfig& pt_config, const std::string& camera = "GoPro") {
  const float max_camera_speed = CAMERA_TYPE_MAX_SPEEDS.at(camera);
  const float scale = pt_config.play_detector.fps_speed_scale;
  const float camera_box_max_speed_x = std::max(arena_box.width() / max_camera_speed, 12.0f);
  const float camera_box_max_speed_y = std::max(arena_box.height() / max_camera_speed, 12.0f);
  const float camera_box_max_accel_x = 1.0 / scale;
  const float camera_box_max_accel_y = 1.0 / scale;
  // Do the "fast" boxes
  for (int i = 0; i < int(pt_config.living_boxes.size()) - 1; ++i) {
    AllLivingBoxConfig& bcfg = pt_config.living_boxes.at(i);
    // Translation
    bcfg.max_speed_x = camera_box_max_speed_x * 1.5 / scale;
    bcfg.max_speed_y = camera_box_max_speed_y * 1.5 / scale;
    bcfg.max_accel_x = camera_box_max_accel_x * 1.1 / scale;
    bcfg.max_accel_y = camera_box_max_accel_y * 1.1 / scale;
    // Resizing
    bcfg.max_speed_w = camera_box_max_speed_x * 1.5 / scale / 1.8;
    bcfg.max_speed_h = camera_box_max_speed_y * 1.5 / scale / 1.8;
    bcfg.max_accel_w = camera_box_max_accel_x * 1.1 / scale;
    bcfg.max_accel_h = camera_box_max_accel_y * 1.1 / scale;
    bcfg.max_width = arena_box.width();
    bcfg.max_height = arena_box.height();
    bcfg.min_height = 10;
    ((ResizingConfig*)&bcfg)->stop_resizing_on_dir_change = false;
    ((TranslatingBoxConfig*)&bcfg)->stop_translation_on_dir_change = false;
    bcfg.sticky_sizing = false;
    bcfg.sticky_translation = false;
    bcfg.arena_box = arena_box;
  }
  // "Do the final box
  if (!pt_config.living_boxes.empty()) {
    AllLivingBoxConfig& bcfg = pt_config.living_boxes.back();
    // Translation
    bcfg.max_speed_x = camera_box_max_speed_x / scale;
    bcfg.max_speed_y = camera_box_max_speed_y / scale;
    bcfg.max_accel_x = camera_box_max_accel_x / scale;
    bcfg.max_accel_y = camera_box_max_accel_y / scale;
    // Resizing
    bcfg.max_speed_w = camera_box_max_speed_x / scale / 1.8;
    bcfg.max_speed_h = camera_box_max_speed_y / scale / 1.8;
    bcfg.max_accel_w = camera_box_max_accel_x / scale;
    bcfg.max_accel_h = camera_box_max_accel_y / scale;
    bcfg.max_width = arena_box.width();
    bcfg.max_height = arena_box.height();
    bcfg.min_height = arena_box.height() / 5;
    ((ResizingConfig*)&bcfg)->stop_resizing_on_dir_change = true;
    ((TranslatingBoxConfig*)&bcfg)->stop_translation_on_dir_change = true;
    bcfg.sticky_sizing = true;
    bcfg.sticky_translation = true;
    bcfg.arena_box = arena_box;
  }
}

PlayTrackerConfig create_play_tracker_config(const BBox& arena_box, const YAML::Node& yaml) {
  PlayTrackerConfig config;
  hm::utils::ConfigLocator locator{
      .ignored{"live-boxes"},
  };

  if (yaml["live-boxes"]) {
    YAML::Node live_boxes = yaml["live-boxes"];
    // Iterate over the list
    for (const auto& box_yaml : live_boxes) {
      config.living_boxes.emplace_back(create_all_living_box_config(arena_box, box_yaml));
    }
    if (!config.living_boxes.empty()) {
      // Last one gets fixed aspect ratio
      config.living_boxes.back().fixed_aspect_ratio = 16.0 / 7.0;
    }
  }
  config.play_detector = create_play_detector_config(yaml, locator);
  adjust_config(arena_box, config);
  SET_LOCATOR(locator, config, no_wide_start);
  SET_LOCATOR(locator, config, max_lost_track_age);
  SET_LOCATOR(locator, config, ignore_largest_bbox);
  set_config_from_yaml(yaml, locator);

  return config;
}

hm::play_tracker::PlayTracker* get_or_create_play_tracker(const BBox& arena_box, DsPlayTrackerCtx* ctx) {
  if (ctx->play_tracker.has_value()) {
    return ctx->play_tracker->get();
  }
  if (!ctx->initParams.play_tracker_config_file.empty()) {
    try {
      YAML::Node yaml = YAML::LoadFile(ctx->initParams.play_tracker_config_file);
      if (yaml["play-tracker"]) {
        ctx->play_tracker_config = create_play_tracker_config(arena_box, yaml["play-tracker"]);
        ctx->play_tracker = std::make_unique<hm::play_tracker::PlayTracker>(arena_box, ctx->play_tracker_config);
        return ctx->play_tracker->get();
      } else {
        g_error("Could not find 'play-tracker' in config file: %s", ctx->initParams.play_tracker_config_file.c_str());
        ctx->play_tracker = nullptr;
      }
    } catch (const std::exception& e) {
      g_error("Error loading YAML file: %s", e.what());
      ctx->play_tracker = nullptr;
    }
  } else {
    ctx->play_tracker = nullptr;
  }

  return nullptr;
}

void plot_resizing_state(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    bool draw_thresholds,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  const hm::play_tracker::LivingState& living_state = lbox->get_live_box_state();
  const hm::play_tracker::ResizingState& resizing_state = lbox->get_resizing_state();
  const hm::play_tracker::TranslationState& translation_state = lbox->get_translation_state();
  (void)living_state;
  (void)resizing_state;
  (void)translation_state;
  if (box_config.sticky_sizing) {
    BBox my_bbox = lbox->bounding_box();
    assert(following_lbox.has_value());
    BBox following_box = following_lbox.value()->bounding_box();
    if (resizing_state.size_is_frozen) {
      // Draw thick corners when frozen
      plotter.plot_corner_rect(my_bbox, /*thickness=*/8, hm::utils::ColorRGB{255, 255, 255}, 0.2, 0.2);
      // BBox corner_box = my_bbox.make_scaled(0.98, 0.98);
      // plotter.plot_corner_rect(corner_box, /*thickness=*/8, hm::utils::ColorRGB{255, 255, 255}, 0.2, 0.2);
    }
    BBox scaled_following_box = following_box.make_scaled(box_config.scale_dest_width, box_config.scale_dest_height);
    BBox inscribed = scaled_following_box.at_center(my_bbox.center());
    int dash_length = inscribed.width() / 5;
    plotter.plot_dashed_rect(
        inscribed,
        /*thickness=*/2,
        hm::utils::ColorRGB{255, 255, 255},
        /*dash_length=*/dash_length,
        /*gap_length=*/dash_length);
    if (draw_thresholds) {
      Point my_center = my_bbox.center();
      auto my_width = my_bbox.width(), my_height = my_bbox.height();
      hm::play_tracker::GrowShrink gs = lbox->get_grow_shrink_wh(my_bbox);
      BBox grow_box =
          BBox(my_center, hm::WHDims{.width = my_width + gs.grow_width, .height = my_height + gs.grow_height});
      BBox shrink_box =
          BBox(my_center, hm::WHDims{.width = my_width - gs.shrink_width, .height = my_height - gs.shrink_height});
      plotter.plot_no_corner_rect(grow_box, /*thickness=*/4, hm::utils::ColorRGB{0, 255, 0}, 0.5, 0.5);
      plotter.plot_no_corner_rect(shrink_box, /*thickness=*/4, hm::utils::ColorRGB{0, 0, 255}, 0.5, 0.5);
    }
  }
}

void plot_living_box(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    bool draw_thresholds,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  plot_resizing_state(plotter, lbox, box_config, draw_thresholds, following_lbox);
  const hm::play_tracker::LivingState& living_state = lbox->get_live_box_state();
  const hm::play_tracker::ResizingState& resizing_state = lbox->get_resizing_state();
  const hm::play_tracker::TranslationState& translation_state = lbox->get_translation_state();
  (void)living_state;
  (void)resizing_state;
  (void)translation_state;
}

} // namespace gst_hm

DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* initParams) {
  DsPlayTrackerCtx* ctx = new DsPlayTrackerCtx();
  ctx->initParams = *initParams;
  return ctx;
}

static const std::array<hm::utils::ColorRGB, 2> track_colors{
    hm::utils::ColorRGB{0, 0, 255},
    hm::utils::ColorRGB{255, 0, 255},
};
static const hm::utils::ColorRGB breakway_edge_line{128, 0, 28};
static const hm::utils::ColorRGB breakway_edge_circle{128, 0, 28};

bool DsPlayTrackerProcessFrame(GstDsPlayTrackerFrame& frame, DsPlayTrackerCtx* ctx) {
  hm::BBox arena_box(0, 0, frame.frame_meta->source_frame_width, frame.frame_meta->source_frame_height);
  hm::play_tracker::PlayTracker* play_tracker = gst_hm::get_or_create_play_tracker(arena_box, ctx);
  if (!play_tracker) {
    return false;
  }

  std::vector<size_t> tracking_ids;
  std::vector<hm::BBox> tracking_boxes;

  const std::size_t object_count = g_list_length(frame.frame_meta->obj_meta_list);
  tracking_ids.reserve(object_count);
  tracking_boxes.reserve(object_count);

  for (NvDsMetaList* l_obj = frame.frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    const NvDsComp_BboxInfo& trackler_bbox_info = obj_meta->tracker_bbox_info;
    tracking_boxes.emplace_back(hm::BBox(
        trackler_bbox_info.org_bbox_coords.left,
        trackler_bbox_info.org_bbox_coords.top,
        trackler_bbox_info.org_bbox_coords.left + trackler_bbox_info.org_bbox_coords.width,
        trackler_bbox_info.org_bbox_coords.top + trackler_bbox_info.org_bbox_coords.height));
    size_t tracking_id = obj_meta->object_id;
    tracking_ids.push_back(tracking_id);
  }

  frame.play_tracker_results = play_tracker->forward(tracking_ids, tracking_boxes);
  if (ctx->initParams.draw) {
    hm::utils::PlotContext plotter(frame.frame_meta, "");
    for (const auto& cluster_item : frame.play_tracker_results.cluster_boxes) {
      plotter.plot_rect(cluster_item.second, 1, hm::utils::ColorRGB{0, 0, 0}, hm::utils::ColorRGBA{128, 128, 128, 32});
    }
    for (size_t i = 0, n = frame.play_tracker_results.tracking_boxes.size(); i < n; ++i) {
      plotter.plot_rect(frame.play_tracker_results.tracking_boxes[i], 5, track_colors.at(i));
      if (ctx->play_tracker.has_value()) {
        std::shared_ptr<hm::play_tracker::ILivingBox> lbox = (*ctx->play_tracker)->get_live_box(i);
        hm::play_tracker::ILivingBox* following_box = i ? (*ctx->play_tracker)->get_live_box(i - 1).get() : nullptr;
        gst_hm::plot_living_box(
            plotter, lbox.get(), ctx->play_tracker_config.living_boxes.at(i), /*draw_thresholds=*/true, following_box);
      }
    }
    if (frame.play_tracker_results.play_detection.has_value()) {
      const hm::play_tracker::PlayDetectorResults& play_detector = *frame.play_tracker_results.play_detection;
      if (play_detector.breakaway_edge_center.has_value()) {
        plotter.plot_circle(
            *play_detector.breakaway_edge_center, /*radius=*/30, /*thickness=*/15, breakway_edge_circle);
        plotter.plot_line(
            frame.play_tracker_results.tracking_boxes.at(0).center(),
            *play_detector.breakaway_edge_center,
            3,
            breakway_edge_line);
      }
    }
  }
  return true;
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

  return out;
}

void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx) {
  if (ctx) {
    if (ctx->play_tracker) {
      ctx->play_tracker->reset();
    }
    delete ctx;
  }
}
