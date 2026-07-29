#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "hockeymom/csrc/play_tracker/PlayTracker.h"
#include "hockeymom/csrc/play_tracker/ResizingBox.h"
#include "hockeymom/csrc/play_tracker/TranslatingBox.h"
#include "hstream/src/gst-plugins/gst-fieldmask/fieldmask_payload.h"
#include "hstream/src/libs/common/ConfigYaml.h"
#include "hstream/src/libs/common/PlotContext.h"

#include <nvdsmeta.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <set>

#include <stdio.h>
#include <stdlib.h>

namespace gst_hm_playtracker {

using namespace hm;
using namespace hm::play_tracker;

bool validate_numeric_yaml_fields(const YAML::Node& node, std::string* error) {
  static const std::set<std::string> kNumericKeys = {
      "arena-angle-from-vertical",
      "dynamic-acceleration-scaling",
      "fps-speed-scale",
      "frame-step",
      "group-ratio-threshold",
      "group-velocity-speed-ratio",
      "max-accel-h",
      "max-accel-w",
      "max-accel-x",
      "max-accel-y",
      "max-lost-track-age",
      "max-positions",
      "max-speed-h",
      "max-speed-w",
      "max-speed-x",
      "max-speed-y",
      "max-velocity-positions",
      "max-width",
      "max-height",
      "min-considered-group-velocity",
      "min-width",
      "min-height",
      "nonstop-delay-count",
      "overshoot-scale-speed-ratio",
      "scale-dest-height",
      "scale-dest-width",
      "scale-speed-constraints",
      "size-ratio-thresh-grow-dh",
      "size-ratio-thresh-grow-dw",
      "size-ratio-thresh-shrink-dh",
      "size-ratio-thresh-shrink-dw",
      "sticky-size-ratio-to-frame-width",
      "sticky-translation-gaussian-mult",
      "unsticky-translation-size-ratio",
  };
  if (!node) {
    return true;
  }
  if (node.IsSequence()) {
    for (const auto& item : node) {
      if (!validate_numeric_yaml_fields(item, error)) {
        return false;
      }
    }
    return true;
  }
  if (!node.IsMap()) {
    return true;
  }
  for (const auto& entry : node) {
    if (!entry.first.IsScalar()) {
      continue;
    }
    std::string key = entry.first.as<std::string>();
    std::replace(key.begin(), key.end(), '_', '-');
    const YAML::Node value = entry.second;
    if (kNumericKeys.count(key) && value && value.IsScalar()) {
      try {
        const double parsed = value.as<double>();
        if (!std::isfinite(parsed)) {
          if (error) {
            *error = absl::StrCat("invalid non-finite numeric value for ", key);
          }
          return false;
        }
      } catch (const std::exception& exc) {
        if (error) {
          *error = absl::StrCat("invalid numeric value for ", key, ": ", exc.what());
        }
        return false;
      }
    }
    if (!validate_numeric_yaml_fields(value, error)) {
      return false;
    }
  }
  return true;
}

PlayDetectorConfig create_play_detector_config(
    PlayDetectorConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
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

ResizingConfig create_resizing_config(
    ResizingConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
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
    TranslatingBoxConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator) {
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
  SET_LOCATOR(locator, config, dynamic_acceleration_scaling);
  SET_LOCATOR(locator, config, arena_angle_from_vertical);
  config.arena_box = arena_box;
  return config;
}

LivingBoxConfig create_living_box_config(
    LivingBoxConfig& config,
    const YAML::Node& yaml,
    hm::utils::ConfigLocator& locator,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  SET_LOCATOR(locator, config, scale_dest_width);
  SET_LOCATOR(locator, config, scale_dest_height);
  SET_LOCATOR(locator, config, clamp_scaled_input_box);
  config.fixed_aspect_ratio = fixed_aspect_ratio;
  return config;
}

void apply_all_living_box_config(
    const BBox& arena_box,
    AllLivingBoxConfig& config,
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  hm::utils::ConfigLocator locator;
  SET_LOCATOR(locator, config, name);
  *((ResizingConfig*)&config) = create_resizing_config(config, yaml, locator);
  *((TranslatingBoxConfig*)&config) = create_translating_box_config(arena_box, config, yaml, locator);
  *((LivingBoxConfig*)&config) = create_living_box_config(config, yaml, locator, fixed_aspect_ratio);
  set_config_from_yaml(yaml, locator);
}

AllLivingBoxConfig create_all_living_box_config(
    const BBox& arena_box,
    const YAML::Node& yaml,
    std::optional<FloatValue> fixed_aspect_ratio = std::nullopt) {
  AllLivingBoxConfig config;
  apply_all_living_box_config(arena_box, config, yaml, fixed_aspect_ratio);
  return config;
}

const std::unordered_map<std::string, float> CAMERA_TYPE_MAX_SPEEDS = {
    {"GoPro", 200.0},
    {"Zhiwei", 200.0},
    {"LiveBarn", 300.0},
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

    bcfg.dynamic_acceleration_scaling = true; // EXPERIMENTA
    // bcfg.dynamic_acceleration_scaling = false;
  }
}

PlayTrackerConfig create_play_tracker_config(const BBox& arena_box, const YAML::Node& yaml) {
  PlayTrackerConfig config;
  hm::utils::ConfigLocator locator{
      .ignored{"live-boxes"},
  };
  std::vector<YAML::Node> live_box_yamls;

  if (yaml["live-boxes"]) {
    YAML::Node live_boxes = yaml["live-boxes"];
    // Iterate over the list
    for (const auto& box_yaml : live_boxes) {
      live_box_yamls.emplace_back(box_yaml);
      config.living_boxes.emplace_back(create_all_living_box_config(arena_box, box_yaml));
    }
    if (!config.living_boxes.empty()) {
      // Last one gets fixed aspect ratio
      config.living_boxes.back().fixed_aspect_ratio = 16.0 / 9.0;
    }
  }
  config.play_detector = create_play_detector_config(config.play_detector, yaml, locator);

  config.ignore_outlier_players = true; // EXPERIMENTAL
  config.ignore_left_and_right_extremes = false; // EXPERIMENTAL

  adjust_config(arena_box, config);
  for (size_t i = 0; i < live_box_yamls.size() && i < config.living_boxes.size(); ++i) {
    const std::optional<FloatValue> fixed_aspect_ratio =
        i + 1 == config.living_boxes.size() ? std::optional<FloatValue>(16.0 / 9.0) : std::nullopt;
    apply_all_living_box_config(arena_box, config.living_boxes[i], live_box_yamls[i], fixed_aspect_ratio);
  }
  SET_LOCATOR(locator, config, no_wide_start);
  SET_LOCATOR(locator, config, max_lost_track_age);
  SET_LOCATOR(locator, config, ignore_largest_bbox);
  set_config_from_yaml(yaml, locator);

  config.no_wide_start = true;

  return config;
}

bool has_play_tracker(DsPlayTrackerCtx* ctx, int source_id) {
  return !!ctx->play_trackers.count(source_id);
}

hm::play_tracker::PlayTracker* get_play_tracker(DsPlayTrackerCtx* ctx, int source_id) {
  return ctx->play_trackers.at(source_id).play_tracker.get();
}

hm::play_tracker::PlayTracker* get_or_create_play_tracker(DsPlayTrackerCtx* ctx, int source_id, const BBox& arena_box) {
  // std::cerr << "play tracker source_id = " << source_id << std::endl;
  if (has_play_tracker(ctx, source_id)) {
    return get_play_tracker(ctx, source_id);
  }
  if (!ctx->initParams.play_tracker_config_file.empty()) {
    try {
      YAML::Node yaml = YAML::LoadFile(ctx->initParams.play_tracker_config_file);
      if (yaml["play-tracker"]) {
        ctx->play_trackers[source_id].play_tracker_config = create_play_tracker_config(arena_box, yaml["play-tracker"]);
        ctx->play_trackers[source_id].play_tracker = std::make_unique<hm::play_tracker::PlayTracker>(
            arena_box, ctx->play_trackers[source_id].play_tracker_config);
        return ctx->play_trackers[source_id].play_tracker.get();
      } else {
        g_error("Could not find 'play-tracker' in config file: %s", ctx->initParams.play_tracker_config_file.c_str());
        ctx->play_trackers[source_id].play_tracker = nullptr;
      }
    } catch (const std::exception& e) {
      g_error("Error loading YAML file: %s", e.what());
      ctx->play_trackers[source_id].play_tracker = nullptr;
    }
  } else {
    ctx->play_trackers[source_id].play_tracker = nullptr;
  }

  return nullptr;
}

void plot_progress_bar(
    hm::utils::PlotContext& plotter,
    const hm::BBox& bbox,
    float filled_ratio,
    const hm::utils::ColorRGB& line_color,
    const hm::utils::ColorRGB& unfilled_color,
    const hm::utils::ColorRGB& fill_color) {
  constexpr int kThickness = 1;
  plotter.plot_rect(bbox, /*thickness=*/kThickness, line_color, /*fill_color=*/unfilled_color);
  hm::BBox inner_rect = bbox.deflate(kThickness, kThickness);
  hm::FloatValue ww = inner_rect.width() * std::abs(filled_ratio);
  inner_rect.right = std::min(inner_rect.right, inner_rect.left + ww);
  if (inner_rect.width() <= 0 || inner_rect.height() <= 0) {
    return;
  }
  plotter.plot_rect(inner_rect, /*thickness=*/0, fill_color, fill_color);
}

void plot_resizing_state(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    bool draw_thresholds,
    double scale_width,
    double scale_height,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  const hm::play_tracker::ResizingState& resizing_state = lbox->get_resizing_state();
  if (box_config.sticky_sizing) {
    BBox my_bbox = lbox->bounding_box().make_canvas_scaled(scale_width, scale_height);
    assert(following_lbox.has_value());
    BBox following_box = following_lbox.value()->bounding_box().make_canvas_scaled(scale_width, scale_height);
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

void plot_translation_state(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    int thickness,
    const hm::utils::ColorT& color,
    bool draw_thresholds,
    double scale_width,
    double scale_height,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  const hm::play_tracker::TranslationState& translation_state = lbox->get_translation_state();
  // (void)translation_state;
  BBox my_bbox = lbox->bounding_box().make_canvas_scaled(scale_width, scale_height);
  plotter.plot_rect(
      my_bbox, thickness, translation_state.translation_is_frozen ? hm::utils::ColorRGB{128, 128, 128} : color);
  if (draw_thresholds && box_config.sticky_translation) {
    const auto sticky_unsticky = lbox->get_sticky_translation_sizes();
    const float scale_ratio = std::sqrt(scale_width * scale_height + scale_width * scale_height);
    const float sticky = std::get<0>(sticky_unsticky) * scale_ratio;
    const float unsticky = std::get<1>(sticky_unsticky) * scale_ratio;
    Point my_center = my_bbox.center();
    plotter.plot_circle(my_center, /*radius=*/int(sticky), /*thickness=*/3, hm::utils::ColorRGB{255, 0, 0});
    plotter.plot_circle(my_center, /*radius=*/int(unsticky), /*thickness=*/3, hm::utils::ColorRGB{255, 0, 255});
    if (following_lbox.has_value()) {
      BBox following_bbox = (*following_lbox)->bounding_box().make_canvas_scaled(scale_width, scale_height);
      Point following_bbox_center = following_bbox.center();
      plotter.plot_circle(
          my_center, /*radius=*/5, /*thickness=*/1, hm::utils::ColorRGB{255, 255, 0}, hm::utils::ColorRGB{255, 255, 0});
      plotter.plot_circle(
          following_bbox_center,
          /*radius=*/5,
          /*thickness=*/1,
          hm::utils::ColorRGB{0, 255, 128},
          hm::utils::ColorRGB{0, 255, 128});
      // Diagonal
      plotter.plot_line(my_center, following_bbox_center, /*thickness=*/10, hm::utils::ColorRGB{255, 0, 0});
      // X shaft
      plotter.plot_line(
          my_center,
          Point{.x = following_bbox_center.x, .y = my_center.y},
          /*thickness=*/3,
          hm::utils::ColorRGB{255, 255, 0});
      // Y shaft
      plotter.plot_line(
          my_center,
          Point{.x = my_center.x, .y = following_bbox_center.y},
          /*thickness=*/3,
          hm::utils::ColorRGB{255, 255, 0});
      // Translation edge scale
      hm::BBox prog(my_center, hm::WHDims{.width = sticky / 2, .height = 25});
      plot_progress_bar(
          plotter,
          prog,
          translation_state.last_arena_edge_center_position_scale,
          hm::utils::ColorRGB{128, 128, 128},
          hm::utils::ColorRGB{64, 64, 64},
          hm::utils::ColorRGB{128, 255, 255});
    }
  }
}

void plot_living_box(
    hm::utils::PlotContext& plotter,
    const ILivingBox* lbox,
    const hm::play_tracker::AllLivingBoxConfig& box_config,
    int thickness,
    const hm::utils::ColorT& color,
    bool draw_thresholds,
    double scale_width,
    double scale_height,
    std::optional<ILivingBox*> following_lbox = std::nullopt) {
  plot_translation_state(
      plotter, lbox, box_config, thickness, color, draw_thresholds, scale_width, scale_height, following_lbox);
  plot_resizing_state(plotter, lbox, box_config, draw_thresholds, scale_width, scale_height, following_lbox);
}

const std::array<hm::utils::ColorRGB, 2> track_colors{
    hm::utils::ColorRGB{0, 0, 255},
    hm::utils::ColorRGB{255, 0, 255},
};
const hm::utils::ColorRGB breakway_edge_line{128, 0, 28};
const hm::utils::ColorRGB breakway_edge_circle{128, 0, 28};

} // namespace gst_hm_playtracker

namespace {
struct ScaleXY {
  double scale_x{1.0};
  double scale_y{1.0};
};

ScaleXY get_scale_xy(const GstDsPlayTrackerFrame& frame) {
  double pipeline_width = frame.input_surf_params->width;
  double pipeline_height = frame.input_surf_params->height;
  const double scale_x = double(frame.frame_meta->source_frame_width) / pipeline_width;
  const double scale_y = double(frame.frame_meta->source_frame_height) / pipeline_height;
  return ScaleXY{.scale_x = scale_x, .scale_y = scale_y};
}

} // namespace

DsPlayTrackerCtx* DsPlayTrackerCtxInit(DsPlayTrackerInitParams* initParams) {
  DsPlayTrackerCtx* ctx = new DsPlayTrackerCtx();
  ctx->initParams = *initParams;
  return ctx;
}

absl::Status DsPlayTrackerValidateConfigFile(const std::string& config_file) {
  if (config_file.empty()) {
    return absl::InvalidArgumentError("playtracker config-file is empty");
  }
  try {
    YAML::Node yaml = YAML::LoadFile(config_file);
    if (!yaml["play-tracker"]) {
      return absl::InvalidArgumentError(absl::StrCat("missing play-tracker in config file: ", config_file));
    }
    YAML::Node live_boxes = yaml["play-tracker"]["live-boxes"];
    if (!live_boxes || !live_boxes.IsSequence() || live_boxes.size() == 0) {
      return absl::InvalidArgumentError("playtracker config play-tracker.live-boxes must be a non-empty sequence");
    }
    std::string numeric_error;
    if (!gst_hm_playtracker::validate_numeric_yaml_fields(yaml["play-tracker"], &numeric_error)) {
      return absl::InvalidArgumentError(numeric_error);
    }
    (void)gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 1920, 1080), yaml["play-tracker"]);
  } catch (const std::exception& exc) {
    return absl::InvalidArgumentError(absl::StrCat("invalid playtracker config file: ", exc.what()));
  }
  return absl::OkStatus();
}

bool DsPlayTrackerProcessFrame(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame, cudaStream_t stream) {
  // We always do our calculations wrt the original image, since we tune based upon the camera
  // type, which is generally tied to the resolution. We scale in the play tracker when possible, but
  // it isn't perfectly scalable atm.

  DsPlayTrackerCtx::PlayTracker* play_tracker_ctx{nullptr};

#if 1 && !defined(NDEBUG)
    hm::utils::PlotContext plot_context(frame.frame_meta);
    plot_context.plot_rect(
        //hm::BBox(field_box.x, field_box.y, field_box.x + field_box.width, field_box.y + field_box.height),
        ctx->arena_box,
        20,
        hm::utils::ColorRGB{255, 0, 0});
#endif


  if (!gst_hm_playtracker::has_play_tracker(ctx, frame.frame_meta->source_id)) {
    ctx->arena_box = hm::BBox(0, 0, frame.frame_meta->source_frame_width, frame.frame_meta->source_frame_height);
#ifdef HAS_NVDS_CUSTOMUSERMETA
    const hm::fieldmask::FieldMaskPayload* fieldmask_payload =
        hm::fieldmask::FieldMaskPayload::get_payload<hm::fieldmask::FieldMaskPayload>(frame.frame_meta);
    if (fieldmask_payload) {
      const cv::Rect2i& field_box = fieldmask_payload->field_box();
      float horizontal_expand_ratio = 0.04;
      float horizontal_padding = horizontal_expand_ratio * field_box.width;
      float new_left = field_box.x - horizontal_padding;
      if (new_left < ctx->arena_box.left) {
        new_left = ctx->arena_box.left;
      }
      float new_right = (field_box.x + field_box.width) + horizontal_padding;
      if (new_right > ctx->arena_box.right) {
        new_right = ctx->arena_box.right;
      }
      // Inflate and only apply left and right
      ctx->arena_box = hm::BBox(new_left, ctx->arena_box.top, new_right, ctx->arena_box.bottom);
#if 0 && !defined(NDEBUG)
      plot_context.plot_rect(
          ctx->arena_box,
          20,
          hm::utils::ColorRGB{0, 255, 128});
#endif
    }
#endif 
    gst_hm_playtracker::get_or_create_play_tracker(ctx, frame.frame_meta->source_id, ctx->arena_box);
    play_tracker_ctx = &ctx->play_trackers.at(frame.frame_meta->source_id);
  } else {
    play_tracker_ctx = &ctx->play_trackers.at(frame.frame_meta->source_id);
  }
  if (!play_tracker_ctx || !play_tracker_ctx->play_tracker) {
    return false;
  }
  hm::play_tracker::PlayTracker* play_tracker = play_tracker_ctx->play_tracker.get();

  std::vector<size_t> tracking_ids;
  std::vector<hm::BBox> tracking_boxes;

  const std::size_t object_count = g_list_length(frame.frame_meta->obj_meta_list);
  tracking_ids.reserve(object_count);
  tracking_boxes.reserve(object_count);

  // assert(frame.frame_meta->pipeline_width && frame.frame_meta->pipeline_height);

  ScaleXY scale_xy = get_scale_xy(frame);
  const auto& scale_x = scale_xy.scale_x;
  const auto& scale_y = scale_xy.scale_y;

  for (NvDsMetaList* l_obj = frame.frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* obj_meta = (NvDsObjectMeta*)(l_obj->data);
    if (obj_meta->class_id != 0) {
      continue;
    }
    if (obj_meta->object_id == UNTRACKED_OBJECT_ID) {
      // ignore untracked objects
      continue;
    }
    const NvDsComp_BboxInfo& tracker_bbox_info = obj_meta->tracker_bbox_info;
    tracking_boxes.emplace_back(hm::BBox(
        tracker_bbox_info.org_bbox_coords.left * scale_x,
        tracker_bbox_info.org_bbox_coords.top * scale_y,
        (tracker_bbox_info.org_bbox_coords.left + tracker_bbox_info.org_bbox_coords.width) * scale_x,
        (tracker_bbox_info.org_bbox_coords.top + tracker_bbox_info.org_bbox_coords.height) * scale_y));
    size_t tracking_id = obj_meta->object_id;
    tracking_ids.push_back(tracking_id);
  }

  if (tracking_boxes.empty() && !play_tracker_ctx->has_received_tracks) {
    hm::play_tracker::PlayTrackerResults waiting_results;
    const float frame_width = frame.frame_meta->source_frame_width > 0 ? frame.frame_meta->source_frame_width
                                                                       : frame.frame_meta->pipeline_width;
    const float frame_height = frame.frame_meta->source_frame_height > 0 ? frame.frame_meta->source_frame_height
                                                                         : frame.frame_meta->pipeline_height;
    waiting_results.tracking_boxes.emplace_back(hm::BBox(0, 0, frame_width, frame_height));
    frame.play_tracker_results = std::move(waiting_results);
    DsPlayTrackerAttachMetadataFullFrame(frame.frame_meta, frame.play_tracker_results);
    return true;
  }

  if (!tracking_boxes.empty()) {
    play_tracker_ctx->has_received_tracks = true;
  }

  frame.play_tracker_results = play_tracker->forward(tracking_ids, tracking_boxes);
  if (ctx->initParams.draw) {
    if (!DsPlayTrackerDrawToDisplayMeta(ctx, frame).ok()) {
      return false;
    }
  }
  DsPlayTrackerAttachMetadataFullFrame(frame.frame_meta, frame.play_tracker_results);
  return true;
}

absl::Status DsPlayTrackerDrawToDisplayMeta(DsPlayTrackerCtx* ctx, GstDsPlayTrackerFrame& frame) {
  ScaleXY scale_xy = get_scale_xy(frame);
  const auto& scale_x = scale_xy.scale_x;
  const auto& scale_y = scale_xy.scale_y;

  hm::utils::PlotContext plotter(frame.frame_meta, "");
  // Plot any nontrivial arena box
  if (ctx->arena_box.left > 0 || ctx->arena_box.top > 0 ||
      ctx->arena_box.width() < frame.frame_meta->source_frame_width ||
      ctx->arena_box.height() < frame.frame_meta->source_frame_height) {
    plotter.plot_rect(ctx->arena_box, 2, hm::utils::ColorRGBA{255, 64, 64, 255});
  }
  for (const auto& cluster_item : frame.play_tracker_results.cluster_boxes) {
    plotter.plot_rect(
        cluster_item.second.make_canvas_scaled(1.0 / scale_x, 1.0 / scale_y),
        1,
        hm::utils::ColorRGB{0, 0, 0},
        hm::utils::ColorRGBA{128, 128, 128, 75});
  }
  for (size_t i = 0, n = frame.play_tracker_results.tracking_boxes.size(); i < n; ++i) {
    // plotter.plot_rect(frame.play_tracker_results.tracking_boxes[i], 5, track_colors.at(i));
    auto& play_tracker_ctx = ctx->play_trackers[frame.frame_meta->source_id];
    if (play_tracker_ctx.play_tracker) {
      std::shared_ptr<hm::play_tracker::ILivingBox> lbox = play_tracker_ctx.play_tracker->get_live_box(i);
      hm::play_tracker::ILivingBox* following_box =
          i ? play_tracker_ctx.play_tracker->get_live_box(i - 1).get() : nullptr;

      // We scale back down for drawing, which is on the pipeline image

      gst_hm_playtracker::plot_living_box(
          plotter,
          lbox.get(),
          play_tracker_ctx.play_tracker_config.living_boxes.at(i),
          /*thickness=*/4,
          gst_hm_playtracker::track_colors.at(i),
          /*draw_thresholds=*/true,
          1.0 / scale_x,
          1.0 / scale_y,
          following_box);
    }
  }
  if (frame.play_tracker_results.play_detection.has_value()) {
    const hm::play_tracker::PlayDetectorResults& play_detector = *frame.play_tracker_results.play_detection;
    if (play_detector.breakaway_edge_center.has_value()) {
      plotter.plot_circle(
          *play_detector.breakaway_edge_center,
          /*radius=*/30,
          /*thickness=*/15,
          gst_hm_playtracker::breakway_edge_circle);
      plotter.plot_line(
          frame.play_tracker_results.tracking_boxes.at(0).center(),
          *play_detector.breakaway_edge_center,
          3,
          gst_hm_playtracker::breakway_edge_line);
    }
  }
  // Finally, print the translation scaling value
  // frame.play_tracker_results.
  return absl::OkStatus();
}

/**
 * Attach metadata for the full frame. We will be adding a new metadata.
 */
void DsPlayTrackerAttachMetadataFullFrame(
    NvDsFrameMeta* frame_meta,
    const hm::play_tracker::PlayTrackerResults& play_results) {
  NvDsBatchMeta* batch_meta = frame_meta->base_meta.batch_meta;
  NvDsObjectMeta* object_meta = NULL;

  size_t adder = 0;
  // Start with base vlass id being the last following box
  for (int64_t i = play_results.tracking_boxes.size() - 1; i >= 0; --i, ++adder) {
    const hm::BBox& tracking_box = play_results.tracking_boxes[i];
    object_meta = nvds_acquire_obj_meta_from_pool(batch_meta);
    object_meta->class_id = DsPlayTrackerInitParams::kPlayBoxClassIdBase + adder;

    NvOSD_RectParams& rect_params = object_meta->rect_params;

    // Assign bounding box coordinates
    rect_params.left = tracking_box.left;
    rect_params.top = tracking_box.top;
    rect_params.width = tracking_box.width();
    rect_params.height = tracking_box.height();

    rect_params.border_width = 0;
    rect_params.border_color = (NvOSD_ColorParams){1, 1, 0, 1};

    object_meta->object_id = UNTRACKED_OBJECT_ID;

    nvds_add_obj_meta_to_frame(frame_meta, object_meta, NULL);
  }
}

void DsPlayTrackerCtxDeinit(DsPlayTrackerCtx* ctx) {
  if (ctx) {
    ctx->play_trackers.clear();
    delete ctx;
  }
}
