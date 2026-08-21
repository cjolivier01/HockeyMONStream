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
#include <sstream>
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker_payload.h"
#include "hstream/src/libs/common/DecodedFrameSequenceMeta.h"
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

std::string runtime_tuning_group(const DsPlayTrackerRuntimeTuning& tuning) {
  std::ostringstream group;
  group << tuning.apply_to_fast_box << ':' << tuning.apply_to_follower_box << ':' << tuning.update_motion_tuning;
  auto append_field = [&group](const char* name, bool present) {
    if (present) {
      group << ':' << name;
    }
  };
  append_field("stop-on-dir-change-delay", tuning.stop_on_dir_change_delay.has_value());
  append_field("cancel-on-opposite", tuning.cancel_on_opposite.has_value());
  append_field("cancel-hysteresis-frames", tuning.cancel_hysteresis_frames.has_value());
  append_field("stop-delay-cooldown-frames", tuning.stop_delay_cooldown_frames.has_value());
  append_field("post-nonstop-stop-delay-count", tuning.post_nonstop_stop_delay_count.has_value());
  append_field("time-to-dest-speed-limit-frames", tuning.time_to_dest_speed_limit_frames.has_value());
  append_field("overshoot-stop-delay-count", tuning.overshoot_stop_delay_count.has_value());
  append_field("overshoot-scale-speed-ratio", tuning.overshoot_scale_speed_ratio.has_value());
  append_field("max-speed-x", tuning.max_speed_x.has_value());
  append_field("max-speed-y", tuning.max_speed_y.has_value());
  append_field("max-accel-x", tuning.max_accel_x.has_value());
  append_field("max-accel-y", tuning.max_accel_y.has_value());
  append_field("zoom-in-aggressiveness", tuning.zoom_in_aggressiveness.has_value());
  append_field("arena-angle-from-vertical", tuning.arena_angle_from_vertical.has_value());
  append_field("dynamic-acceleration-scaling", tuning.dynamic_acceleration_scaling.has_value());
  return group.str();
}

DsPlayTrackerRuntimeTuning camera_geometry_tuning(
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
  return tuning;
}

} // namespace

PlayTrackerPriv::~PlayTrackerPriv() {
  telemetry_csv_.Stop();
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
  if (play_tracker_config_source_contents_.empty()) {
    play_tracker_config_source_contents_ = ReadTelemetryConfigArtifact(play_tracker_config_source_file_);
  }
  if (play_tracker_config_source_contents_.empty()) {
    return absl::InvalidArgumentError("could not read non-empty vpplaytracker base configuration snapshot");
  }

  {
    std::lock_guard<std::mutex> lk(context_mu_);
    HM_RETURN_IF_ERROR(ReloadContextFromConfig(play_tracker_config_source_contents_));
  }
  const absl::Status status = Super::PostCapsInit(params);
  if (!status.ok()) {
    return status;
  }
  if (!telemetry_csv_dir_.empty()) {
    const absl::Status telemetry_status = telemetry_csv_.Start(
        telemetry_csv_dir_,
        TelemetryConfigArtifact{play_tracker_config_source_file_, play_tracker_config_source_contents_},
        TelemetryConfigArtifact{init_params_.play_tracker_config_file, play_tracker_effective_config_contents_},
        runtime_tuning_provenance_history_);
    if (!telemetry_status.ok()) {
      return telemetry_status;
    }
    std::cout << "Playtracker telemetry CSV export: " << telemetry_csv_.output_manifest() << std::endl;
  }
  return absl::OkStatus();
}

absl::Status PlayTrackerPriv::ReloadContextFromConfig(const std::string& config_contents) {
  if (play_tracker_config_source_file_.empty()) {
    return absl::NotFoundError("vpplaytracker config-file is not set");
  }

  DsPlayTrackerInitParams next_params = init_params_;
  next_params.owned_objects.clear();
  YAML::Node config;
  try {
    config = YAML::Load(config_contents);
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
  std::ostringstream effective_config;
  effective_config << config;
  const std::string next_effective_config_contents = effective_config.str();
  std::ofstream ofile(temp_yaml_file->getPath());
  ofile << next_effective_config_contents;
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
  for (const DsPlayTrackerRuntimeTuning& tuning : runtime_tuning_history_) {
    const absl::Status status = DsPlayTrackerCtxApplyRuntimeTuning(next_context, tuning);
    if (!status.ok()) {
      DsPlayTrackerCtxDeinit(next_context);
      return status;
    }
  }
  if (pt_context_) {
    DsPlayTrackerCtxDeinit(pt_context_);
  }
  init_params_ = std::move(next_params);
  play_tracker_effective_config_contents_ = next_effective_config_contents;
  pt_context_ = next_context;
  return absl::OkStatus();
}

void PlayTrackerPriv::RememberRuntimeTuning(DsPlayTrackerRuntimeTuning tuning, TelemetryConfigEvent provenance) {
  const std::string group = runtime_tuning_group(tuning);
  const auto previous_group = std::find(runtime_tuning_groups_.begin(), runtime_tuning_groups_.end(), group);
  if (previous_group != runtime_tuning_groups_.end()) {
    const size_t index = static_cast<size_t>(std::distance(runtime_tuning_groups_.begin(), previous_group));
    runtime_tuning_groups_.erase(previous_group);
    runtime_tuning_history_.erase(runtime_tuning_history_.begin() + index);
    runtime_tuning_provenance_history_.erase(runtime_tuning_provenance_history_.begin() + index);
  }
  runtime_tuning_groups_.push_back(group);
  runtime_tuning_history_.push_back(std::move(tuning));
  runtime_tuning_provenance_history_.push_back(provenance);
  if (telemetry_csv_.active()) {
    telemetry_csv_.TryRecordConfigEvent(std::move(provenance));
  }
}

bool PlayTrackerPriv::SetProperty(const Property& prop) {
  std::string key = prop.key;
  std::replace(key.begin(), key.end(), '_', '-');
  auto apply_camera_geometry_locked = [this](const DsPlayTrackerRuntimeTuning& tuning) {
    return !pt_context_ || DsPlayTrackerCtxApplyRuntimeTuning(pt_context_, tuning).ok();
  };
  if (key == "show") {
    show_ = !!std::atol(prop.value.c_str());
  } else if (key == "draw") {
    init_params_.draw = !!std::atol(prop.value.c_str());
  } else if (key == "telemetry-csv-dir") {
    if (telemetry_csv_.active() && prop.value != telemetry_csv_dir_) {
      std::cerr << "telemetry-csv-dir can only be changed before the pipeline starts" << std::endl;
      return false;
    }
    telemetry_csv_dir_ = prop.value;
  } else if (key == "fixed-edge-rotation-angle") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    DsPlayTrackerRuntimeTuning tuning = camera_geometry_tuning(angle, std::nullopt, true, true);
    if (!apply_camera_geometry_locked(tuning)) {
      return false;
    }
    fixed_edge_rotation_angle_left_ = angle;
    fixed_edge_rotation_angle_right_ = angle;
    RememberRuntimeTuning(
        std::move(tuning), {"property", key, prop.value, /*artifact_stem=*/{}, /*artifact_contents=*/{}});
  } else if (key == "fixed-edge-rotation-angle-left") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    DsPlayTrackerRuntimeTuning tuning =
        camera_geometry_tuning(0.5f * (angle + fixed_edge_rotation_angle_right_), std::nullopt, true, true);
    if (!apply_camera_geometry_locked(tuning)) {
      return false;
    }
    fixed_edge_rotation_angle_left_ = angle;
    RememberRuntimeTuning(
        std::move(tuning), {"property", key, prop.value, /*artifact_stem=*/{}, /*artifact_contents=*/{}});
  } else if (key == "fixed-edge-rotation-angle-right") {
    float angle = 0.0f;
    if (!parse_finite_float(prop.value, &angle)) {
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    DsPlayTrackerRuntimeTuning tuning =
        camera_geometry_tuning(0.5f * (fixed_edge_rotation_angle_left_ + angle), std::nullopt, true, true);
    if (!apply_camera_geometry_locked(tuning)) {
      return false;
    }
    fixed_edge_rotation_angle_right_ = angle;
    RememberRuntimeTuning(
        std::move(tuning), {"property", key, prop.value, /*artifact_stem=*/{}, /*artifact_contents=*/{}});
  } else if (key == "dynamic-acceleration-scaling") {
    float dynamic_acceleration_scaling = 0.0f;
    if (!parse_finite_float(prop.value, &dynamic_acceleration_scaling)) {
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    DsPlayTrackerRuntimeTuning tuning = camera_geometry_tuning(std::nullopt, dynamic_acceleration_scaling, false, true);
    if (!apply_camera_geometry_locked(tuning)) {
      return false;
    }
    dynamic_acceleration_scaling_ = dynamic_acceleration_scaling;
    RememberRuntimeTuning(
        std::move(tuning), {"property", key, prop.value, /*artifact_stem=*/{}, /*artifact_contents=*/{}});
  } else if (key == "runtime-tuning-config-file") {
    // Parse outside the streaming mutex so disk I/O and YAML conversion never
    // stall GenerateOutput(). Parsing and provenance both consume this one
    // immutable byte snapshot, so a concurrent pathname replacement cannot
    // make the applied policy differ from the recorded artifact.
    const std::string tuning_contents = ReadTelemetryConfigArtifact(prop.value);
    auto tuning = DsPlayTrackerLoadRuntimeTuningContents(tuning_contents);
    if (!tuning.ok()) {
      std::cerr << tuning.status() << std::endl;
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    if (pt_context_) {
      const absl::Status status = DsPlayTrackerCtxApplyRuntimeTuning(pt_context_, *tuning);
      if (!status.ok()) {
        std::cerr << status << std::endl;
        return false;
      }
    }
    RememberRuntimeTuning(*tuning, {"runtime-tuning", key, prop.value, "play_tracker_runtime_tuning", tuning_contents});
    return true;
  } else if (key == "config-file") {
    // Cache one immutable snapshot for both a live reload and the next startup.
    // Runtime tuning uses runtime-tuning-config-file so active history is preserved.
    const std::string config_contents = ReadTelemetryConfigArtifact(prop.value);
    if (config_contents.empty()) {
      return false;
    }
    std::lock_guard<std::mutex> lk(context_mu_);
    const std::string previous_config_source_file = play_tracker_config_source_file_;
    const std::string previous_config_source_contents = play_tracker_config_source_contents_;
    play_tracker_config_source_file_ = prop.value;
    play_tracker_config_source_contents_ = config_contents;
    if (pt_context_) {
      const absl::Status status = ReloadContextFromConfig(config_contents);
      if (!status.ok()) {
        play_tracker_config_source_file_ = previous_config_source_file;
        play_tracker_config_source_contents_ = previous_config_source_contents;
        std::cerr << status << std::endl;
        return false;
      }
    }
    telemetry_csv_.TryRecordConfigEvent({"base-config", key, prop.value, "play_tracker_source", config_contents});
    return true;
  }
  return true;
}

BufferResult PlayTrackerPriv::ProcessBuffer(GstBuffer* inbuf) {
  return Super::ProcessBuffer(inbuf);
}

bool PlayTrackerPriv::HandleEvent(GstEvent* event) {
  const GstStructure* structure = event ? gst_event_get_structure(event) : nullptr;
  if (structure && gst_structure_has_name(structure, "hstream-playtracker-telemetry-failed")) {
    telemetry_csv_.MarkRunOutcome(TelemetryRunOutcome::kFailed);
    return Super::HandleEvent(event);
  }
  if (structure && gst_structure_has_name(structure, "hstream-playtracker-telemetry-eos")) {
    telemetry_csv_.MarkRunOutcome(TelemetryRunOutcome::kEndOfStream);
    return Super::HandleEvent(event);
  }
  if (event && GST_EVENT_TYPE(event) == GST_EVENT_EOS) {
    return Super::HandleEvent(event);
  }
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
    ++telemetry_seek_epoch_;
    telemetry_csv_.TryRecordDiscontinuity(
        {"seek", "flush-stop", std::to_string(telemetry_seek_epoch_), /*artifact_stem=*/{}, /*artifact_contents=*/{}});
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
    TelemetrySample telemetry_sample;
    const bool export_telemetry = telemetry_csv_.active();
    if (export_telemetry) {
      telemetry_sample.source_id = frame.frame_meta->source_id;
      telemetry_sample.source_frame = frame.frame_meta->frame_num;
      telemetry_sample.seek_epoch = telemetry_seek_epoch_;
      telemetry_sample.width = frame.frame_meta->source_frame_width;
      telemetry_sample.height = frame.frame_meta->source_frame_height;
      if (frame.frame_meta->buf_pts != GST_CLOCK_TIME_NONE) {
        telemetry_sample.pts_ns = frame.frame_meta->buf_pts;
      }
      if (frame.frame_meta->ntp_timestamp != 0) {
        telemetry_sample.ntp_ns = frame.frame_meta->ntp_timestamp;
      }
      const std::optional<DecodedFrameSequence> decoded_sequence = hm::decoded_frame_sequence(frame.frame_meta);
      if (decoded_sequence.has_value()) {
        telemetry_sample.decoded_source_id = decoded_sequence->source_id;
        telemetry_sample.decoded_sequence = decoded_sequence->sequence;
      }

      const double scale_x = frame.input_surf_params->width > 0
          ? static_cast<double>(frame.frame_meta->source_frame_width) / frame.input_surf_params->width
          : 1.0;
      const double scale_y = frame.input_surf_params->height > 0
          ? static_cast<double>(frame.frame_meta->source_frame_height) / frame.input_surf_params->height
          : 1.0;
      for (NvDsMetaList* item = frame.frame_meta->obj_meta_list; item; item = item->next) {
        const auto* object = static_cast<const NvDsObjectMeta*>(item->data);
        if (!object || object->class_id != 0 || object->object_id == UNTRACKED_OBJECT_ID) {
          continue;
        }
        const NvBbox_Coords& bbox = object->tracker_bbox_info.org_bbox_coords;
        const float score = object->tracker_confidence >= 0.0f ? object->tracker_confidence : object->confidence;
        telemetry_sample.tracks.push_back(
            TelemetryTrack{
                object->object_id,
                static_cast<float>(bbox.left * scale_x),
                static_cast<float>(bbox.top * scale_y),
                static_cast<float>(bbox.width * scale_x),
                static_cast<float>(bbox.height * scale_y),
                score,
                object->class_id,
            });
      }
    }
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
    if (export_telemetry) {
      telemetry_sample.policy_boxes.reserve(frame.play_tracker_results.tracking_boxes.size());
      for (const hm::BBox& box : frame.play_tracker_results.tracking_boxes) {
        telemetry_sample.policy_boxes.push_back(
            TelemetryBox{
                static_cast<float>(box.left),
                static_cast<float>(box.top),
                static_cast<float>(box.width()),
                static_cast<float>(box.height()),
            });
      }
      if (!telemetry_csv_.TryEnqueue(std::move(telemetry_sample)) && telemetry_csv_.dropped_samples() == 1) {
        std::cerr << "Playtracker telemetry writer queue is full; dropping complete metadata samples" << std::endl;
      }
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
