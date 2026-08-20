#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker.h"
#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker_payload.h"
#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/TempFile.h"
#include "hstream/src/libs/draw_display/DrawDisplayMeta.h"
#include "nvbufsurface.h"
#include "nvdsmeta.h"

#include <assert.h>
#include <cuda.h>
#include <unistd.h>
#include <yaml-cpp/node/parse.h>

namespace hm {
namespace playtracker {
namespace {

bool parse_finite_float(const std::string& value, float* out) {
  if (!out || value.empty()) {
    return false;
  }
  char* end = nullptr;
  errno = 0;
  const float parsed = std::strtof(value.c_str(), &end);
  if (value.c_str() == end || errno == ERANGE || !std::isfinite(parsed)) {
    return false;
  }
  while (end && *end && std::isspace(static_cast<unsigned char>(*end))) {
    ++end;
  }
  if (!end || *end != '\0') {
    return false;
  }
  *out = parsed;
  return true;
}

} // namespace

PlayTrackerPriv::~PlayTrackerPriv() {
  std::lock_guard<std::mutex> lk(context_mu_);
  if (pt_context_) {
    DsPlayTrackerCtxDeinit(pt_context_);
    pt_context_ = nullptr;
  }
}

absl::Status PlayTrackerPriv::PreCapsInit(DSCustom_CreateParams* params) {
  return Super::PreCapsInit(params);
};

absl::Status PlayTrackerPriv::PostCapsInit(DSCustom_CreateParams* params) {
  // Transform In-Place (but still asynchronously)
  m_transformMode = false;
  // No buffers for us
  params->m_bufferPoolConfig.max_buffers = 0;

  if (params->config_file && play_tracker_config_source_file_.empty()) {
    play_tracker_config_source_file_ = params->config_file;
  }
  if (play_tracker_config_source_file_.empty()) {
    play_tracker_config_source_file_ = init_params_.play_tracker_config_file;
  }

  {
    std::lock_guard<std::mutex> lk(context_mu_);
    HM_RETURN_IF_ERROR(ReloadContextFromConfig());
  }
  return Super::PostCapsInit(params);
}

absl::Status PlayTrackerPriv::ReloadContextFromConfig() {
  if (play_tracker_config_source_file_.empty()) {
    return absl::NotFoundError("vpplaytracker config-file is not set");
  }

  DsPlayTrackerInitParams next_params = init_params_;
  next_params.owned_objects.clear();
  YAML::Node config;
  try {
    config = YAML::LoadFile(play_tracker_config_source_file_);
    YAML::Node live_boxes = config["play-tracker"]["live-boxes"];
    if (!live_boxes || !live_boxes.IsSequence()) {
      return absl::InvalidArgumentError("vpplaytracker config missing play-tracker.live-boxes");
    }
    if (live_boxes.size() == 0) {
      return absl::InvalidArgumentError("vpplaytracker config play-tracker.live-boxes must not be empty");
    }
    const float representative_fixed_edge_rotation_angle =
        0.5f * (fixed_edge_rotation_angle_left_ + fixed_edge_rotation_angle_right_);
    for (YAML::Node box : live_boxes) {
      box["arena-angle-from-vertical"] = std::to_string(representative_fixed_edge_rotation_angle);
    }
    if (live_boxes.size() > 0) {
      live_boxes[live_boxes.size() - 1]["dynamic-acceleration-scaling"] = std::to_string(dynamic_acceleration_scaling_);
    }
  } catch (const std::exception& exc) {
    return absl::InvalidArgumentError(absl::StrCat("failed to load vpplaytracker config: ", exc.what()));
  }
  auto temp_yaml_file = std::make_unique<hm::utils::TempFile>(/*autoRemove=*/true);
  std::ofstream ofile(temp_yaml_file->getPath());
  ofile << config;
  ofile.close();
  if (!ofile) {
    return absl::InternalError("Failed to write generated vpplaytracker runtime config");
  }
  next_params.play_tracker_config_file = temp_yaml_file->getPath();
  next_params.owned_objects.emplace_back(std::move(temp_yaml_file));
  HM_RETURN_IF_ERROR(DsPlayTrackerValidateConfigFile(next_params.play_tracker_config_file));
  DsPlayTrackerCtx* next_context = DsPlayTrackerCtxInit(&next_params);
  if (!next_context) {
    return absl::InternalError("Failed to reload vpplaytracker context");
  }
  if (pt_context_) {
    DsPlayTrackerCtxDeinit(pt_context_);
  }
  init_params_ = std::move(next_params);
  pt_context_ = next_context;
  return absl::OkStatus();
}

bool PlayTrackerPriv::SetProperty(const Property& prop) {
  bool reload_context = false;
  std::string key = prop.key;
  std::replace(key.begin(), key.end(), '_', '-');
  auto apply_camera_geometry = [this](
                                   std::optional<float> angle,
                                   std::optional<float> dynamic_acceleration_scaling,
                                   bool apply_to_fast_box,
                                   bool apply_to_follower_box) {
    DsPlayTrackerRuntimeTuning tuning;
    tuning.apply_to_fast_box = apply_to_fast_box;
    tuning.apply_to_follower_box = apply_to_follower_box;
    tuning.update_motion_tuning = false;
    tuning.arena_angle_from_vertical = angle;
    tuning.dynamic_acceleration_scaling = dynamic_acceleration_scaling;
    std::lock_guard<std::mutex> lk(context_mu_);
    return !pt_context_ || DsPlayTrackerCtxApplyRuntimeTuning(pt_context_, tuning).ok();
  };
  if (key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (key == "draw") {
    init_params_.draw = !!std::atol(prop.value.c_str());
  } else if (key == "fixed-edge-rotation-angle") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    if (!apply_camera_geometry(angle, std::nullopt, true, true)) {
      return false;
    }
    fixed_edge_rotation_angle_left_ = angle;
    fixed_edge_rotation_angle_right_ = angle;
  } else if (key == "fixed-edge-rotation-angle-left") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    if (!apply_camera_geometry(0.5f * (angle + fixed_edge_rotation_angle_right_), std::nullopt, true, true)) {
      return false;
    }
    fixed_edge_rotation_angle_left_ = angle;
  } else if (key == "fixed-edge-rotation-angle-right") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    if (!apply_camera_geometry(0.5f * (fixed_edge_rotation_angle_left_ + angle), std::nullopt, true, true)) {
      return false;
    }
    fixed_edge_rotation_angle_right_ = angle;
  } else if (key == "dynamic-acceleration-scaling") {
    float dynamic_acceleration_scaling = 0.0f;
    if (!parse_finite_float(prop.value, &dynamic_acceleration_scaling)) {
      return false;
    }
    if (!apply_camera_geometry(std::nullopt, dynamic_acceleration_scaling, false, true)) {
      return false;
    }
    dynamic_acceleration_scaling_ = dynamic_acceleration_scaling;
  } else if (key == "runtime-tuning-config-file") {
    // Parse outside the streaming mutex so disk I/O and YAML conversion never
    // stall GenerateOutput(). Only the small in-place mutation is serialized.
    auto tuning = DsPlayTrackerLoadRuntimeTuning(prop.value);
    if (!tuning.ok()) {
      std::cerr << tuning.status() << std::endl;
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    if (!pt_context_) {
      return false;
    }
    const absl::Status status = DsPlayTrackerCtxApplyRuntimeTuning(pt_context_, *tuning);
    if (!status.ok()) {
      std::cerr << status << std::endl;
      return false;
    }
    return true;
  } else if (key == "config-file") {
    // This property changes the next-start base configuration. Runtime tuning
    // uses runtime-tuning-config-file so active tracker history is preserved.
    reload_context = true;
  }
  if (reload_context) {
    std::lock_guard<std::mutex> lk(context_mu_);
    const std::string previous_config_source_file = play_tracker_config_source_file_;
    if (key == "config-file") {
      play_tracker_config_source_file_ = prop.value;
    }
    if (pt_context_) {
      const absl::Status status = ReloadContextFromConfig();
      if (!status.ok()) {
        play_tracker_config_source_file_ = previous_config_source_file;
        std::cerr << status << std::endl;
        return false;
      }
    }
  }
  return true;
}

BufferResult PlayTrackerPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

bool PlayTrackerPriv::HandleEvent(GstEvent* event) {
  if (event && GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
    // Let the async base worker retire every old-generation frame while the
    // src pad is still flushing, then reset tracking before the new segment.
    // Waiting in the opposite order can deadlock with GenerateOutput(), which
    // owns this same context mutex.
    const bool handled = Super::HandleEvent(event);
    std::lock_guard<std::mutex> lk(context_mu_);
    DsPlayTrackerCtxResetTracking(pt_context_);
    prev_play_tracker_results_ = hm::play_tracker::PlayTrackerResults{};
    frame_counter_ = 0;
    return handled;
  }
  return Super::HandleEvent(event);
}

absl::Status PlayTrackerPriv::GenerateOutput(
    NvDsBatchMeta* batch_meta,
    NvBufSurface* in_surface,
    NvBufSurface* /*out_surface*/) {
  std::lock_guard<std::mutex> lk(context_mu_);
  if (!pt_context_) {
    return absl::FailedPreconditionError("vpplaytracker context is not initialized");
  }
  GstDsPlayTrackerFrame frame;
  auto font_cache = draw_display::get_or_create_font_cache();
  NvDsFrameMetaList* fl = batch_meta->frame_meta_list;
  while (fl) {
    assert(frame.batch_index < in_surface->numFilled);
    frame.frame_meta = (NvDsFrameMeta*)fl->data;
    frame.input_surf_params = &in_surface->surfaceList[frame.batch_index];
    if (frame_counter_ % frame_calculation_interval_ == 0) {
      if (!DsPlayTrackerProcessFrame(pt_context_, frame, cuda_stream_)) {
        return absl::InternalError("Error calling DsPlayTrackerProcessFrame()");
      }
#ifdef HAS_USER_APPLICATION_PAYLOAD
      PlayTrackerPayload::create_and_add<PlayTrackerPayload>(frame.frame_meta, pt_context_->arena_box);
#endif
      prev_play_tracker_results_ = frame.play_tracker_results;
    } else {
      assert(false);
      frame.play_tracker_results = prev_play_tracker_results_;
      if (pt_context_->initParams.draw) {
        HM_RETURN_IF_ERROR(DsPlayTrackerDrawToDisplayMeta(pt_context_, frame));
      }
      DsPlayTrackerAttachMetadataFullFrame(frame.frame_meta, frame.play_tracker_results);
    }
    if (show_) {
      NvDisplayMetaList* dm_list = frame.frame_meta->display_meta_list;
      while (dm_list) {
        NvDsDisplayMeta* display_meta = (NvDsDisplayMeta*)dm_list->data;
        HM_RETURN_IF_ERROR(draw_display_meta(frame.input_surf_params, display_meta, font_cache, 1.0f, cuda_stream_));
        dm_list = dm_list->next;
      }
    }
    ++frame.batch_index;
    ++frame_counter_;
    fl = fl->next;
  }
  return absl::OkStatus();
}

} // namespace playtracker
} // namespace hm
