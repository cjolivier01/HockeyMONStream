#include "hstream/src/gst-plugins/gst-playtracker/PlayTrackerCtx.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <unistd.h>
#include <cmath>
#include <fstream>
#include <iostream>

namespace {
struct Output {
  std::vector<hm::BBox> boxes;
  std::optional<hm::BBox> native;
  absl::Status error;
};
Output run(
    DsPlayTrackerCtx* context,
    int tick,
    const std::string& generation,
    bool players = true,
    bool timestamp = true) {
  auto batch = std::unique_ptr<NvDsBatchMeta, decltype(&nvds_destroy_batch_meta)>(
      nvds_create_batch_meta(1), nvds_destroy_batch_meta);
  auto* metadata = nvds_acquire_frame_meta_from_pool(batch.get());
  nvds_add_frame_meta_to_batch(batch.get(), metadata);
  metadata->source_frame_width = metadata->pipeline_width = 7000;
  metadata->source_frame_height = metadata->pipeline_height = 3000;
  metadata->source_id = 0;
  metadata->frame_num = tick;
  metadata->buf_pts = timestamp ? uint64_t(tick) * 16666667 : GST_CLOCK_TIME_NONE;
  if (!hm::stitching::add_stitched_output_generation_meta(metadata, generation))
    throw std::runtime_error("generation metadata");
  for (int i = 0; players && i < 5; ++i) {
    auto* object = nvds_acquire_obj_meta_from_pool(batch.get());
    object->class_id = 0;
    object->object_id = i + 1;
    const float x = 2400 + tick * 7 + i * 90;
    object->tracker_bbox_info.org_bbox_coords = {x, 1250, 50, 200};
    object->rect_params.left = x;
    object->rect_params.top = 1250;
    object->rect_params.width = 50;
    object->rect_params.height = 200;
    nvds_add_obj_meta_to_frame(metadata, object, nullptr);
  }
  NvBufSurfaceParams surface{};
  surface.width = 7000;
  surface.height = 3000;
  GstDsPlayTrackerFrame frame;
  frame.frame_meta = metadata;
  frame.input_surf_params = &surface;
  if (!DsPlayTrackerProcessFrame(context, frame, nullptr)) {
    if (!frame.processing_error.ok())
      return {{}, {}, frame.processing_error};
    throw std::runtime_error("native frame processing");
  }
  return {frame.play_tracker_results.tracking_boxes, frame.native_program};
}
bool same(const hm::BBox& a, const hm::BBox& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}
} // namespace

int main() {
  char path[] = "/tmp/hstream-rink-live-XXXXXX";
  const int fd = mkstemp(path);
  if (fd < 0)
    return 1;
  close(fd);
  std::ofstream(path) << R"(play-tracker:
  camera-name: GoPro
  no-wide-start: 0
  ignore-referees: false
  ignore-goalies: false
  ignore-largest-bbox: false
  max-speed-ratio-x: 1.0
  max-speed-ratio-y: 1.0
  max-accel-ratio-x: 1.0
  max-accel-ratio-y: 1.0
  follower-box-min-height-ratio: 0.3
  zoom-in-aggressiveness: 25
  live-boxes:
    - name: current_roi
    - name: current_roi_aspect
      dynamic-acceleration-scaling: 0
      sticky-translation: false
      sticky-sizing: false
)";
  auto map = std::make_shared<hm::stitching::PlaybackRinkMap>();
  map->fit.state = hm::stitching::RinkMapState::kReady;
  map->fit.plane_to_ray = cv::Matx33d(1, 0, 0, 0, 1, 0, 0, 0, 20);
  map->columns = map->rows = 2;
  map->rays = {{-35, -15, 20}, {35, -15, 20}, {-35, 15, 20}, {35, 15, 20}};
  map->hugin_generation = "rink-live-fixture\n";
  const std::string generation =
      "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(map->hugin_generation.size()) + "\n" +
      map->hugin_generation + "post-stitch-rotate-degrees:0\noutput-size:7000x3000\n";
  DsPlayTrackerInitParams parameters;
  parameters.play_tracker_config_file = path;
  auto native = std::unique_ptr<DsPlayTrackerCtx, decltype(&DsPlayTrackerCtxDeinit)>(
      DsPlayTrackerCtxInit(&parameters), DsPlayTrackerCtxDeinit);
  parameters.rink_camera = {map, {true, false, false}};
  auto guided = std::unique_ptr<DsPlayTrackerCtx, decltype(&DsPlayTrackerCtxDeinit)>(
      DsPlayTrackerCtxInit(&parameters), DsPlayTrackerCtxDeinit);
  if (!native || !guided)
    return 1;
  bool moved = false;
  try {
    for (int tick = 0; tick < 180; ++tick) {
      const auto a = run(native.get(), tick, generation), b = run(guided.get(), tick, generation);
      if (a.boxes.empty() || b.boxes.empty() || !b.native || !same(a.boxes.back(), *b.native) ||
          !same(a.boxes.front(), b.boxes.front()) ||
          std::abs(a.boxes.back().width() - b.boxes.back().width()) > 0.001 ||
          std::abs(a.boxes.back().height() - b.boxes.back().height()) > 0.001)
        throw std::runtime_error("Anticipation changed native state, fast framing or zoom");
      moved |= std::abs(a.boxes.back().left - b.boxes.back().left) > 1;
    }
    const auto b = run(guided.get(), 180, "stale");
    if (!moved || b.error.ok() || b.error.message().find("does not match") == std::string::npos ||
        native->play_trackers.at(0).rink_guide)
      throw std::runtime_error("Stale geometry must fail explicitly, and disabled runs must not prepare a guide");
    DsPlayTrackerCtxResetTracking(guided.get());
    const auto empty = run(guided.get(), 0, "stale", false);
    if (empty.error.ok())
      throw std::runtime_error("Geometry validation must fail on the first frame even before detections arrive");
    const auto untimed = run(guided.get(), 0, generation, false, false);
    if (untimed.error.ok() || untimed.error.message().find("timestamps") == std::string::npos)
      throw std::runtime_error("Missing source time must fail rather than silently disabling the guide");
    DsPlayTrackerCtxResetTracking(guided.get());
    if (!guided->play_trackers.empty())
      throw std::runtime_error("Reset retained guide history");
  } catch (const std::exception& error) {
    unlink(path);
    std::cerr << error.what() << '\n';
    return 1;
  }
  unlink(path);
  return 0;
}
