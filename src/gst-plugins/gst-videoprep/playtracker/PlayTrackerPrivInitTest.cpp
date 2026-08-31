#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker.h"
#include "hstream/src/libs/common/DetectionSnapshotMeta.h"

#include "absl/status/status.h"

#include <cuda_runtime.h>
#include <gst/gst.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

class TestPlayTrackerPriv : public hm::playtracker::PlayTrackerPriv {
 public:
  using PlayTrackerPriv::PlayTrackerPriv;

  void StopTelemetry() {
    telemetry_csv_.Stop();
  }

  float fixedEdgeRotationAngleLeft() const {
    return fixed_edge_rotation_angle_left_;
  }

  const std::string& effectiveConfigContents() const {
    return play_tracker_effective_config_contents_;
  }

  size_t retainedRuntimeTuningCount() const {
    return runtime_tuning_history_.size();
  }

  size_t retainedRuntimeProvenanceCount() const {
    return runtime_tuning_provenance_history_.size();
  }
};

std::string read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

size_t count_occurrences(const std::string& value, const std::string& needle) {
  size_t count = 0;
  for (size_t position = 0; (position = value.find(needle, position)) != std::string::npos; position += needle.size()) {
    ++count;
  }
  return count;
}

bool generate_export_sample(TestPlayTrackerPriv& priv, uint64_t frame_number) {
  NvDsBatchMeta* batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* frame_meta = batch ? nvds_acquire_frame_meta_from_pool(batch) : nullptr;
  NvDsObjectMeta* object_meta = batch ? nvds_acquire_obj_meta_from_pool(batch) : nullptr;
  if (!batch || !frame_meta || !object_meta) {
    if (batch) {
      nvds_destroy_batch_meta(batch);
    }
    return false;
  }
  frame_meta->source_id = 0;
  frame_meta->frame_num = frame_number;
  frame_meta->source_frame_width = 3840;
  frame_meta->source_frame_height = 1080;
  frame_meta->pipeline_width = 3840;
  frame_meta->pipeline_height = 1080;
  object_meta->class_id = 0;
  object_meta->object_id = 1;
  object_meta->unique_component_id = 1;
  object_meta->confidence = 0.8f;
  object_meta->tracker_confidence = 0.9f;
  object_meta->detector_bbox_info.org_bbox_coords = NvBbox_Coords{118.0f, 98.0f, 404.0f, 264.0f};
  object_meta->tracker_bbox_info.org_bbox_coords = NvBbox_Coords{120.0f, 100.0f, 400.0f, 260.0f};
  nvds_add_obj_meta_to_frame(frame_meta, object_meta, nullptr);
  nvds_add_frame_meta_to_batch(batch, frame_meta);
  if (!hm::detection_snapshot::add_meta(batch, 1)) {
    nvds_destroy_batch_meta(batch);
    return false;
  }

  NvBufSurfaceParams surface_params{};
  surface_params.width = 3840;
  surface_params.height = 1080;
  NvBufSurface surface{};
  surface.batchSize = 1;
  surface.numFilled = 1;
  surface.surfaceList = &surface_params;
  const absl::Status status = priv.GenerateOutput(batch, &surface, nullptr);
  nvds_destroy_batch_meta(batch);
  return status.ok();
}

fs::path write_minimal_config(const fs::path& dir) {
  fs::create_directories(dir);
  fs::path cfg = dir / "play_tracker_config.yaml";

  std::ofstream out(cfg);
  out << "play-tracker:\n";
  out << "  camera-name: GoPro\n";
  out << "  no-wide-start: true\n";
  out << "  ignore-largest-bbox: true\n";
  out << "  fps-speed-scale: 1.0\n";
  out << "  min-considered-group-velocity: 3.0\n";
  out << "  group-ratio-threshold: 0.5\n";
  out << "  group-velocity-speed-ratio: 0.3\n";
  out << "  scale-speed-constraints: 3.0\n";
  out << "  nonstop-delay-count: 2\n";
  out << "  overshoot-scale-speed-ratio: 0.7\n";
  out << "  overshoot-stop-delay-count: 6\n";
  out << "  max-speed-ratio-x: 1.0\n";
  out << "  max-speed-ratio-y: 1.0\n";
  out << "  max-accel-ratio-x: 1.0\n";
  out << "  max-accel-ratio-y: 1.0\n";
  out << "  follower-box-min-height-ratio: 0.2\n";
  out << "  zoom-in-aggressiveness: 25\n";
  out << "  live-boxes:\n";
  out << "    - name: current_roi\n";
  out << "      time-to-dest-speed-limit-frames: 20\n";
  out << "      time-to-dest-stop-speed-threshold: 0.25\n";
  out << "      resizing-stop-on-dir-change-delay: 4\n";
  out << "      resizing-cancel-stop-on-opposite-dir: true\n";
  out << "      resizing-stop-cancel-hysteresis-frames: 10\n";
  out << "      resizing-stop-delay-cooldown-frames: 2\n";
  out << "      resizing-time-to-dest-speed-limit-frames: 10\n";
  out << "      resizing-time-to-dest-stop-speed-threshold: 0.25\n";
  out << "    - name: current_roi_aspect\n";
  out << "      stop-translation-on-dir-change-delay: 10\n";
  out << "      cancel-stop-on-opposite-dir: true\n";
  out << "      cancel-stop-hysteresis-frames: 2\n";
  out << "      stop-delay-cooldown-frames: 2\n";
  out << "      post-nonstop-stop-delay-count: 6\n";
  out << "      time-to-dest-speed-limit-frames: 20\n";
  out << "      time-to-dest-stop-speed-threshold: 0.25\n";
  out << "      resizing-stop-on-dir-change-delay: 4\n";
  out << "      resizing-cancel-stop-on-opposite-dir: true\n";
  out << "      resizing-stop-cancel-hysteresis-frames: 10\n";
  out << "      resizing-stop-delay-cooldown-frames: 2\n";
  out << "      resizing-time-to-dest-speed-limit-frames: 10\n";
  out << "      resizing-time-to-dest-stop-speed-threshold: 0.25\n";
  out << "      sticky-size-ratio-to-frame-width: 10.0\n";
  out << "      sticky-translation-gaussian-mult: 5.0\n";
  out << "      unsticky-translation-size-ratio: 0.75\n";
  out << "      scale-dest-width: 1.45\n";
  out << "      scale-dest-height: 1.45\n";
  return cfg;
}

fs::path write_runtime_config(const fs::path& dir) {
  fs::path cfg = dir / "runtime_tuning.yaml";
  std::ofstream out(cfg);
  out << "play-tracker:\n";
  out << "  hstream-apply-to-fast-box: false\n";
  out << "  hstream-apply-to-follower-box: true\n";
  out << "  overshoot-stop-delay-count: 9\n";
  out << "  overshoot-scale-speed-ratio: 0.45\n";
  out << "  hstream-runtime-tuning:\n";
  out << "    stop-translation-on-dir-change-delay: 7\n";
  out << "    cancel-stop-on-opposite-dir: true\n";
  out << "    cancel-stop-hysteresis-frames: 3\n";
  out << "    stop-delay-cooldown-frames: 4\n";
  out << "    post-nonstop-stop-delay-count: 5\n";
  out << "    time-to-dest-speed-limit-frames: 22\n";
  out << "    overshoot-stop-delay-count: 9\n";
  out << "    overshoot-scale-speed-ratio: 0.45\n";
  out << "    max-speed-x: 31.0\n";
  out << "    max-speed-y: 17.0\n";
  out << "    max-accel-x: 2.0\n";
  out << "    max-accel-y: 1.5\n";
  out << "    zoom-in-aggressiveness: 75\n";
  out << "  live-boxes:\n";
  out << "    - name: current_roi\n";
  out << "    - name: current_roi_aspect\n";
  out << "      stop-translation-on-dir-change-delay: 7\n";
  out << "      cancel-stop-on-opposite-dir: true\n";
  out << "      cancel-stop-hysteresis-frames: 3\n";
  out << "      stop-delay-cooldown-frames: 4\n";
  out << "      post-nonstop-stop-delay-count: 5\n";
  out << "      time-to-dest-speed-limit-frames: 22\n";
  out << "      max-speed-x: 31.0\n";
  out << "      max-speed-y: 17.0\n";
  out << "      max-accel-x: 2.0\n";
  out << "      max-accel-y: 1.5\n";
  return cfg;
}

fs::path write_sparse_runtime_config(
    const fs::path& dir,
    const std::string& name,
    const std::string& key,
    float value) {
  fs::path cfg = dir / name;
  std::ofstream out(cfg);
  out << "play-tracker:\n";
  out << "  hstream-apply-to-fast-box: false\n";
  out << "  hstream-apply-to-follower-box: true\n";
  out << "  hstream-runtime-tuning:\n";
  out << "    " << key << ": " << value << "\n";
  out << "  live-boxes:\n";
  out << "    - name: current_roi\n";
  out << "    - name: current_roi_aspect\n";
  return cfg;
}

fs::path write_both_boxes_runtime_config(const fs::path& dir, const std::string& name, float max_speed_x) {
  fs::path cfg = dir / name;
  std::ofstream out(cfg);
  out << "play-tracker:\n";
  out << "  hstream-apply-to-fast-box: true\n";
  out << "  hstream-apply-to-follower-box: true\n";
  out << "  hstream-runtime-tuning:\n";
  out << "    max-speed-x: " << max_speed_x << "\n";
  out << "  live-boxes:\n";
  out << "    - name: current_roi\n";
  out << "    - name: current_roi_aspect\n";
  return cfg;
}

fs::path write_geometry_runtime_config(
    const fs::path& dir,
    const std::string& name,
    float angle,
    float dynamic_acceleration_scaling) {
  fs::path cfg = dir / name;
  std::ofstream out(cfg);
  out << "play-tracker:\n";
  out << "  hstream-apply-to-fast-box: false\n";
  out << "  hstream-apply-to-follower-box: true\n";
  out << "  hstream-runtime-tuning:\n";
  out << "    arena-angle-from-vertical: " << angle << "\n";
  out << "    dynamic-acceleration-scaling: " << dynamic_acceleration_scaling << "\n";
  out << "  live-boxes:\n";
  out << "    - name: current_roi\n";
  out << "    - name: current_roi_aspect\n";
  return cfg;
}

} // namespace

int main() {
  gst_init(nullptr, nullptr);

  const fs::path tmpdir = fs::temp_directory_path() / "vpplaytracker_priv_init_test";
  fs::remove_all(tmpdir);
  const fs::path cfg = write_minimal_config(tmpdir);
  const fs::path runtime_cfg = write_runtime_config(tmpdir);
  const std::string cfg_str = cfg.string();
  const std::string original_cfg_contents = read_file(cfg);
  const std::string original_runtime_contents = read_file(runtime_cfg);

  cudaStream_t stream = nullptr;
  const cudaError_t cuda_err = cudaStreamCreate(&stream);
  if (cuda_err != cudaSuccess || stream == nullptr) {
    std::cerr << "cudaStreamCreate failed: " << cudaGetErrorString(cuda_err) << std::endl;
    return 10;
  }

  TestPlayTrackerPriv priv(/*gpu_id=*/0, /*batch_size=*/1);
  const fs::path telemetry_dir = tmpdir / "telemetry";
  if (!priv.SetProperty(hm::Property("telemetry-csv-dir", telemetry_dir.string()))) {
    std::cerr << "vpplaytracker rejected telemetry directory before caps initialization\n";
    return 19;
  }

  hm::DSCustom_CreateParams params{};
  params.config_file = const_cast<char*>(cfg_str.c_str());
  params.m_gpuId = 0;
  params.m_cudaStream = stream;
  params.m_inCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720,framerate=30/1");
  params.m_outCaps = gst_caps_from_string("video/x-raw,format=RGBA,width=1280,height=720,framerate=30/1");

  absl::Status status = priv.PreCapsInit(&params);
  if (!status.ok()) {
    std::cerr << "PreCapsInit failed: " << status << std::endl;
    return 1;
  }
  if (!priv.SetProperty(hm::Property("config-file", cfg.string()))) {
    std::cerr << "vpplaytracker rejected base config snapshot before caps initialization\n";
    return 24;
  }
  if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", runtime_cfg.string()))) {
    std::cerr << "vpplaytracker rejected runtime tuning before caps initialization\n";
    return 18;
  }

  std::string rewritten_cfg_contents = original_cfg_contents;
  const size_t zoom_position = rewritten_cfg_contents.find("zoom-in-aggressiveness: 25");
  if (zoom_position == std::string::npos) {
    std::cerr << "could not prepare base config replacement regression\n";
    return 25;
  }
  rewritten_cfg_contents.replace(
      zoom_position, std::string("zoom-in-aggressiveness: 25").size(), "zoom-in-aggressiveness: 5");
  std::ofstream(cfg, std::ios::trunc) << rewritten_cfg_contents;
  std::string rewritten_runtime_contents = original_runtime_contents;
  const size_t runtime_speed_position = rewritten_runtime_contents.find("max-speed-x: 31.0");
  if (runtime_speed_position == std::string::npos) {
    std::cerr << "could not prepare runtime config replacement regression\n";
    return 26;
  }
  rewritten_runtime_contents.replace(
      runtime_speed_position, std::string("max-speed-x: 31.0").size(), "max-speed-x: 99.0");
  std::ofstream(runtime_cfg, std::ios::trunc) << rewritten_runtime_contents;

  status = priv.PostCapsInit(&params);
  if (!status.ok()) {
    std::cerr << "PostCapsInit failed: " << status << std::endl;
    return 2;
  }
  const YAML::Node effective_snapshot = YAML::Load(priv.effectiveConfigContents());
  if (effective_snapshot["play-tracker"]["zoom-in-aggressiveness"].as<int>() != 25) {
    std::cerr << "base config pathname replacement changed the applied immutable snapshot\n";
    return 27;
  }
  std::ofstream(cfg, std::ios::trunc) << original_cfg_contents;
  std::ofstream(runtime_cfg, std::ios::trunc) << original_runtime_contents;

  DsPlayTrackerCtx* context = priv.contextForTesting();
  if (!context) {
    std::cerr << "vpplaytracker context missing after initialization\n";
    return 3;
  }

  NvDsBatchMeta* initial_batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* initial_frame_meta = initial_batch ? nvds_acquire_frame_meta_from_pool(initial_batch) : nullptr;
  if (!initial_batch || !initial_frame_meta) {
    std::cerr << "could not allocate initial-frame metadata\n";
    if (initial_batch) {
      nvds_destroy_batch_meta(initial_batch);
    }
    return 14;
  }
  initial_frame_meta->source_id = 0;
  initial_frame_meta->source_frame_width = 3840;
  initial_frame_meta->source_frame_height = 1080;
  initial_frame_meta->pipeline_width = 3840;
  initial_frame_meta->pipeline_height = 1080;
  nvds_add_frame_meta_to_batch(initial_batch, initial_frame_meta);
  NvBufSurfaceParams initial_surface{};
  initial_surface.width = 3840;
  initial_surface.height = 1080;
  GstDsPlayTrackerFrame initial_frame;
  initial_frame.frame_meta = initial_frame_meta;
  initial_frame.input_surf_params = &initial_surface;
  const bool initial_processed = DsPlayTrackerProcessFrame(context, initial_frame, stream);
  const auto initial_tracker = context->play_trackers.find(0);
  const bool initial_frame_preserved = initial_processed && initial_batch->num_frames_in_batch == 1 &&
      initial_batch->frame_meta_list && initial_batch->frame_meta_list->data == initial_frame_meta;
  const bool waiting_without_synthetic_crop = initial_frame.play_tracker_results.tracking_boxes.empty() &&
      initial_frame_meta->obj_meta_list == nullptr && initial_tracker != context->play_trackers.end() &&
      !initial_tracker->second.has_received_tracks;
  nvds_destroy_batch_meta(initial_batch);
  if (!initial_frame_preserved || !waiting_without_synthetic_crop) {
    std::cerr << "pre-detection frame was dropped or given a synthetic full-frame Program crop\n";
    return 15;
  }

  YAML::Node base_yaml = YAML::LoadFile(cfg.string());
  context->play_trackers[0].play_tracker_config =
      gst_hm_playtracker::create_play_tracker_config(hm::BBox(0, 0, 1280, 720), base_yaml["play-tracker"]);
  context->play_trackers[0].base_play_tracker_config = context->play_trackers[0].play_tracker_config;
  context->play_trackers[0].play_tracker = std::make_unique<hm::play_tracker::PlayTracker>(
      hm::BBox(0, 0, 1280, 720), context->play_trackers[0].play_tracker_config);
  context->play_trackers[0].has_received_tracks = true;
  auto* tracker_before = context->play_trackers[0].play_tracker.get();
  const float fast_dynamic_scaling_before =
      context->play_trackers[0].play_tracker_config.living_boxes[0].dynamic_acceleration_scaling;
  if (!priv.SetProperty(hm::Property("fixed-edge-rotation-angle", "31.0")) ||
      context->play_trackers[0].play_tracker_config.living_boxes[0].dynamic_acceleration_scaling !=
          fast_dynamic_scaling_before ||
      context->play_trackers[0].play_tracker_config.living_boxes[0].arena_angle_from_vertical != 31.0f ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].arena_angle_from_vertical != 31.0f) {
    std::cerr << "fixed-edge rotation changed fast-box dynamic acceleration scaling\n";
    return 11;
  }
  if (!priv.SetProperty(hm::Property("dynamic-acceleration-scaling", "2.5")) ||
      context->play_trackers[0].play_tracker_config.living_boxes[0].dynamic_acceleration_scaling !=
          fast_dynamic_scaling_before ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].dynamic_acceleration_scaling != 2.5f) {
    std::cerr << "dynamic acceleration scaling did not remain follower-specific\n";
    return 12;
  }
  tracker_before->set_bboxes({hm::BBox(120, 100, 520, 360), hm::BBox(80, 60, 720, 420)});
  const hm::BBox follower_before = tracker_before->get_live_box(1)->bounding_box();
  if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", runtime_cfg.string()))) {
    std::cerr << "vpplaytracker rejected valid runtime tuning config\n";
    return 4;
  }
  const hm::BBox follower_after = tracker_before->get_live_box(1)->bounding_box();
  if (priv.contextForTesting() != context || context->play_trackers[0].play_tracker.get() != tracker_before ||
      !context->play_trackers[0].has_received_tracks || follower_after.left != follower_before.left ||
      follower_after.top != follower_before.top || follower_after.right != follower_before.right ||
      follower_after.bottom != follower_before.bottom) {
    std::cerr << "runtime tuning replaced or reset active tracker state\n";
    return 5;
  }
  if (priv.SetProperty(hm::Property("runtime-tuning-config-file", (tmpdir / "missing.yaml").string()))) {
    std::cerr << "vpplaytracker accepted invalid runtime tuning config\n";
    return 6;
  }
  const auto& base_follower = context->play_trackers[0].base_play_tracker_config.living_boxes[1];
  const fs::path sparse_x = write_sparse_runtime_config(tmpdir, "runtime_x.yaml", "max-speed-x", 41.0f);
  const fs::path sparse_y = write_sparse_runtime_config(tmpdir, "runtime_y.yaml", "max-speed-y", 23.0f);
  const fs::path reset_x = write_sparse_runtime_config(tmpdir, "runtime_reset_x.yaml", "max-speed-x", 0.0f);
  if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", sparse_x.string())) ||
      !priv.SetProperty(hm::Property("runtime-tuning-config-file", sparse_y.string()))) {
    std::cerr << "vpplaytracker rejected sequential sparse runtime tuning\n";
    return 7;
  }
  const auto& sequential = context->play_trackers[0].play_tracker_config.living_boxes[1];
  if (sequential.max_speed_x != 41.0f || sequential.max_speed_y != 23.0f) {
    std::cerr << "sequential sparse runtime tuning discarded an earlier override\n";
    return 8;
  }
  if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", reset_x.string())) ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].max_speed_x != base_follower.max_speed_x ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].max_speed_y != 23.0f) {
    std::cerr << "runtime zero reset did not restore only the requested configured value\n";
    return 9;
  }
  const fs::path both_boxes_tuning = write_both_boxes_runtime_config(tmpdir, "runtime_both.yaml", 55.0f);
  const fs::path both_boxes_reset = write_both_boxes_runtime_config(tmpdir, "runtime_both_reset.yaml", 0.0f);
  if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", both_boxes_tuning.string())) ||
      context->play_trackers[0].play_tracker_config.living_boxes[0].max_speed_x != 55.0f ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].max_speed_x != 55.0f ||
      !priv.SetProperty(hm::Property("runtime-tuning-config-file", both_boxes_reset.string())) ||
      context->play_trackers[0].play_tracker_config.living_boxes[0].max_speed_x !=
          context->play_trackers[0].base_play_tracker_config.living_boxes[0].max_speed_x ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].max_speed_x !=
          context->play_trackers[0].base_play_tracker_config.living_boxes[1].max_speed_x) {
    std::cerr << "both-box runtime reset did not restore fast and follower configured values\n";
    return 13;
  }

  if (!generate_export_sample(priv, 1) || !priv.SetProperty(hm::Property("fixed-edge-rotation-angle-left", "32.0")) ||
      priv.fixedEdgeRotationAngleLeft() != 32.0f ||
      context->play_trackers[0].play_tracker_config.living_boxes[0].arena_angle_from_vertical != 31.5f ||
      context->play_trackers[0].play_tracker_config.living_boxes[1].arena_angle_from_vertical != 31.5f ||
      !generate_export_sample(priv, 2)) {
    std::cerr << "geometry state/event update did not precede the next exported sample\n";
    return 20;
  }

  GstEvent* flush_stop = gst_event_new_flush_stop(FALSE);
  const bool flush_handled = priv.HandleEvent(flush_stop);
  gst_event_unref(flush_stop);
  if (!flush_handled || priv.contextForTesting() != context || !context->play_trackers.empty()) {
    std::cerr << "flushing seek did not clear tracker history in place\n";
    return 16;
  }
  NvDsBatchMeta* post_seek_batch = nvds_create_batch_meta(1);
  NvDsFrameMeta* post_seek_frame_meta = nvds_acquire_frame_meta_from_pool(post_seek_batch);
  post_seek_frame_meta->source_id = 0;
  post_seek_frame_meta->source_frame_width = 3840;
  post_seek_frame_meta->source_frame_height = 1080;
  nvds_add_frame_meta_to_batch(post_seek_batch, post_seek_frame_meta);
  NvBufSurfaceParams post_seek_surface{};
  post_seek_surface.width = 3840;
  post_seek_surface.height = 1080;
  GstDsPlayTrackerFrame post_seek_frame;
  post_seek_frame.frame_meta = post_seek_frame_meta;
  post_seek_frame.input_surf_params = &post_seek_surface;
  const bool post_seek_processed = DsPlayTrackerProcessFrame(context, post_seek_frame, stream);
  const auto post_seek_tracker = context->play_trackers.find(0);
  const bool zoom_tuning_restored = post_seek_tracker != context->play_trackers.end() &&
      std::abs(post_seek_tracker->second.play_tracker_config.living_boxes.back().size_ratio_thresh_shrink_dw - 0.032f) <
          0.0001f;
  nvds_destroy_batch_meta(post_seek_batch);
  if (!post_seek_processed || !zoom_tuning_restored) {
    std::cerr << "post-seek tracker did not restore accumulated live zoom tuning\n";
    return 17;
  }

  const float fast_dynamic_before_chronology =
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[0].dynamic_acceleration_scaling;
  const fs::path runtime_geometry = write_geometry_runtime_config(tmpdir, "runtime_geometry.yaml", 44.0f, 4.0f);
  if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", runtime_geometry.string())) ||
      !priv.SetProperty(hm::Property("fixed-edge-rotation-angle", "35.0")) ||
      !priv.SetProperty(hm::Property("dynamic-acceleration-scaling", "2.75")) ||
      !priv.SetProperty(hm::Property("config-file", cfg.string())) || !generate_export_sample(priv, 3)) {
    std::cerr << "could not verify runtime-geometry then property replay ordering\n";
    return 32;
  }
  context = priv.contextForTesting();
  if (!context || context->play_trackers.find(0) == context->play_trackers.end() ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[0].arena_angle_from_vertical != 35.0f ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[1].arena_angle_from_vertical != 35.0f ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[0].dynamic_acceleration_scaling !=
          fast_dynamic_before_chronology ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[1].dynamic_acceleration_scaling !=
          2.75f) {
    std::cerr << "later geometry property was lost during base-config reload\n";
    return 33;
  }

  write_geometry_runtime_config(tmpdir, runtime_geometry.filename().string(), 47.0f, 5.0f);
  if (!priv.SetProperty(hm::Property("fixed-edge-rotation-angle", "36.0")) ||
      !priv.SetProperty(hm::Property("dynamic-acceleration-scaling", "3.0")) ||
      !priv.SetProperty(hm::Property("runtime-tuning-config-file", runtime_geometry.string())) ||
      !priv.SetProperty(hm::Property("config-file", cfg.string())) || !generate_export_sample(priv, 4)) {
    std::cerr << "could not verify property then runtime-geometry replay ordering\n";
    return 34;
  }
  context = priv.contextForTesting();
  if (!context || context->play_trackers.find(0) == context->play_trackers.end() ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[0].arena_angle_from_vertical != 36.0f ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[1].arena_angle_from_vertical != 47.0f ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[0].dynamic_acceleration_scaling !=
          fast_dynamic_before_chronology ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[1].dynamic_acceleration_scaling != 5.0f) {
    std::cerr << "later runtime geometry was lost during base-config reload\n";
    return 35;
  }

  const fs::path repeated_tuning = tmpdir / "runtime_repeated_x.yaml";
  const size_t retained_tuning_count = priv.retainedRuntimeTuningCount();
  float latest_repeated_speed = 0.0f;
  for (size_t update = 0; update < 128; ++update) {
    latest_repeated_speed = 60.0f + static_cast<float>(update);
    write_sparse_runtime_config(tmpdir, repeated_tuning.filename().string(), "max-speed-x", latest_repeated_speed);
    if (!priv.SetProperty(hm::Property("runtime-tuning-config-file", repeated_tuning.string()))) {
      std::cerr << "vpplaytracker rejected repeated same-group runtime tuning\n";
      return 28;
    }
  }
  if (priv.retainedRuntimeTuningCount() != retained_tuning_count ||
      priv.retainedRuntimeProvenanceCount() != retained_tuning_count) {
    std::cerr << "same-group runtime tuning grew retained replay/provenance state\n";
    return 29;
  }
  if (!priv.SetProperty(hm::Property("config-file", cfg.string())) || !generate_export_sample(priv, 5)) {
    std::cerr << "could not reload the tracker to verify compacted runtime tuning replay\n";
    return 30;
  }
  context = priv.contextForTesting();
  if (!context || context->play_trackers.find(0) == context->play_trackers.end() ||
      context->play_trackers.find(0)->second.play_tracker_config.living_boxes[1].max_speed_x != latest_repeated_speed) {
    std::cerr << "base-config reload did not replay the latest compacted runtime tuning\n";
    return 31;
  }

  GstEvent* eos = gst_event_new_eos();
  const bool eos_handled = priv.HandleEvent(eos);
  gst_event_unref(eos);
  if (!eos_handled) {
    std::cerr << "EOS did not mark telemetry outcome\n";
    return 22;
  }
  GstEvent* pipeline_eos = gst_event_new_custom(
      GST_EVENT_CUSTOM_DOWNSTREAM_OOB, gst_structure_new_empty("hstream-playtracker-telemetry-eos"));
  const bool pipeline_eos_handled = priv.HandleEvent(pipeline_eos);
  gst_event_unref(pipeline_eos);
  if (!pipeline_eos_handled) {
    std::cerr << "Pipeline EOS did not mark telemetry outcome\n";
    return 23;
  }
  priv.StopTelemetry();
  const std::string config_events = read_file(telemetry_dir / "hstream_config_events.csv");
  const std::string telemetry_manifest = read_file(telemetry_dir / "hstream_telemetry.json");
  if (config_events.find("1,1,runtime-tuning,runtime-tuning-config-file,") == std::string::npos ||
      read_file(telemetry_dir / "play_tracker_runtime_tuning-1.yaml") != original_runtime_contents ||
      read_file(telemetry_dir / "play_tracker_source.yaml") != original_cfg_contents ||
      config_events.find(",11,property,fixed-edge-rotation-angle-left,32.0,") == std::string::npos ||
      config_events.find(",13,seek,flush-stop,1,") == std::string::npos ||
      count_occurrences(config_events, repeated_tuning.string()) != 128 ||
      telemetry_manifest.find("\"run_outcome\": \"end-of-stream\"") == std::string::npos ||
      telemetry_manifest.find("\"eligible_for_training\": true") == std::string::npos) {
    std::cerr << "geometry or seek event was not committed at the correct attempted-sample boundary\n";
    return 21;
  }

  if (params.m_inCaps) {
    gst_caps_unref(params.m_inCaps);
    params.m_inCaps = nullptr;
  }
  if (params.m_outCaps) {
    gst_caps_unref(params.m_outCaps);
    params.m_outCaps = nullptr;
  }

  cudaStreamDestroy(stream);
  return 0;
}
