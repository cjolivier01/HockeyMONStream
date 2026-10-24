#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "hstream/src/libs/stitching/StitchedOutputGenerationPayload.h"

#include <gstnvdsmeta.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
namespace pa = hm::player_analytics;
namespace preview = hm::preview_overlay;
namespace gpu = hm::gpu_preview;
std::string rink_status_messages;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

// Real collect_preview_overlays caller, metadata only: no GL context, video
// readback, compositor substitution or reproduction of its selection algorithm.
GstBuffer* Buffer(bool transform, uint32_t baked, bool ignored = false, bool snapshot = true) {
  auto* batch = nvds_create_batch_meta(1);
  Check(batch, "batch allocation failed");
  auto* frame = nvds_acquire_frame_meta_from_pool(batch);
  Check(frame, "frame allocation failed");
  frame->source_id = 4;
  frame->buf_pts = 11 * pa::kSecond;
  frame->source_frame_width = 640;
  frame->source_frame_height = 480;
  nvds_add_frame_meta_to_batch(batch, frame);
  pa::FrameResult semantic;
  semantic.stream_id = 4;
  semantic.pts_ns = frame->buf_pts;
  semantic.coordinate_width = 640;
  semantic.coordinate_height = 480;
  semantic.player_count = 1;
  auto& player = semantic.players[0];
  player.track_id = (uint64_t{1} << 40) + 7;
  player.box = {220, 150, 100, 180};
  player.has_pose = true;
  player.pose_observed_at = frame->buf_pts;
  player.pose[0] = {240, 180, 0.9F};
  player.color_slot = 3;
  player.jersey.text = {'0', '7', '\0'};
  player.jersey.confidence = 1;
  player.jersey.evidence = 2;
  player.jersey.observed_at = 10 * pa::kSecond;
  player.jersey.expires_at = 13 * pa::kSecond;
  player.action = {28, 0.9F, pa::kSecond, 10900000000, 12900000000};
  std::strcpy(player.action_text.data(), "reading");
  Check(pa::AttachFrameResult(frame, semantic), "semantic metadata attachment failed");
  auto* object = nvds_acquire_obj_meta_from_pool(batch);
  Check(object, "object allocation failed");
  object->class_id = 0;
  object->object_id = player.track_id;
  object->rect_params.left = 220;
  object->rect_params.top = 150;
  object->rect_params.width = 100;
  object->rect_params.height = 180;
  object->rect_params.border_color = {1, 0, 0, 1};
  preview::set_player_ignored(*object, ignored);
  nvds_add_obj_meta_to_frame(frame, object, nullptr);
  if (snapshot) {
    Check(preview::add_overlay_snapshot_meta(frame), "snapshot attachment failed");
    // Preview must use the frozen decision even after downstream mutation.
    preview::set_player_ignored(*object, false);
  }
  if (transform) {
    preview::PlayCropperTransform crop{
        320, 240, 640, 480, 40, 0, 90, 105, 0, 30, 180, 150, 160, 120, 3.1875F, false, baked};
    Check(preview::add_playcropper_transform_meta(frame, crop), "transform attachment failed");
  }
  auto* buffer = gst_buffer_new();
  auto* meta =
      gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
  Check(meta, "GStreamer batch attachment failed");
  meta->meta_type = NVDS_BATCH_GST_META;
  return buffer;
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
#if !defined(__x86_64__)
  std::cout << "SKIP: HmGpuPreview is only implemented on x86_64\n";
  return 0;
#else
  try {
    Check(gpu::register_elements(), "preview registration failed");
    auto* sink = gst_element_factory_make("hmgpupreviewsink", "player-overlay-inspection");
    Check(sink, "preview sink creation failed");
    constexpr uint32_t layers = pa::kDrawPose | pa::kDrawJerseys | pa::kDrawActions;
    g_object_set(sink, "channel", "program", "show-player-tracking", TRUE, nullptr);
    gpu::configure_player_analytics(sink, layers, 0.3F);
    GstBuffer* missing = Buffer(false, 0);
    GstBuffer* fresh = Buffer(true, 0);
    GstBuffer* baked = Buffer(true, layers | pa::kDrawPlayerBoxes);
    const auto first = gpu::inspect_preview_overlays_for_test(sink, fresh);
    Check(
        first.diagnostic_coordinates_valid && first.analytics_command_count >= 3 && first.path_count > 0,
        "actual preview caller omitted requested unbaked semantics");
    const auto lost = gpu::inspect_preview_overlays_for_test(sink, missing);
    Check(
        !lost.diagnostic_coordinates_valid && lost.analytics_command_count == 0 && lost.path_count == 0,
        "missing Program transform leaked old or untransformed semantic commands");
    const auto already = gpu::inspect_preview_overlays_for_test(sink, baked);
    Check(
        already.diagnostic_coordinates_valid && already.analytics_command_count == 0 && already.path_count == 0,
        "Program preview redrew baked semantics or player boxes");
    g_object_set(sink, "channel", "stitched", nullptr);
    const auto stitched = gpu::inspect_preview_overlays_for_test(sink, baked);
    Check(
        stitched.analytics_command_count == first.analytics_command_count && stitched.path_count > 0,
        "Program baked transform suppressed independent Stitched semantics");
    gpu::configure_player_analytics(sink, 0, 0.3F);
    const auto disabled = gpu::inspect_preview_overlays_for_test(sink, fresh);
    Check(
        disabled.analytics_command_count == 0 && disabled.path_count > 0,
        "disabled semantic drawing retained commands or disabled independent box diagnostics");
    for (bool snapshot : {true, false}) {
      auto* ignored = Buffer(true, 0, true, snapshot);
      for (const char* channel : {"stitched", "program"}) {
        g_object_set(sink, "channel", channel, "show-player-tracking", TRUE, nullptr);
        const auto marked = gpu::inspect_preview_overlays_for_test(sink, ignored);
        Check(marked.analytics_command_count == 7 && marked.path_count > 0, "box-only preview omitted IGNORED label");
        for (const auto& color : marked.colors)
          Check(color[0] == .5 && color[1] == .5 && color[2] == .5 && color[3] == 1, "preview ignored box is not gray");
        g_object_set(sink, "show-player-tracking", FALSE, nullptr);
        const auto hidden = gpu::inspect_preview_overlays_for_test(sink, ignored);
        Check(
            hidden.analytics_command_count == 0 && hidden.path_count == 0,
            "player box switch left ignored overlays visible");
      }
      gst_buffer_unref(ignored);
    }
    g_object_set(sink, "channel", "program", "show-player-tracking", TRUE, nullptr);
    auto* ignored_baked = Buffer(true, pa::kDrawPlayerBoxes, true);
    const auto marked_baked = gpu::inspect_preview_overlays_for_test(sink, ignored_baked);
    Check(
        marked_baked.analytics_command_count == 0 && marked_baked.path_count == 0,
        "Program preview repeated baked ignored overlays");
    gst_buffer_unref(ignored_baked);
    auto map = std::make_shared<hm::stitching::PlaybackRinkMap>();
    map->hugin_generation = "fixture\n";
    map->fit.state = hm::stitching::RinkMapState::kReady;
    map->fit.plane_to_ray = cv::Matx33d(0, -1, 19, -1, 0, -3, 0, 0, -7);
    map->columns = map->rows = 2;
    map->rays.assign(4, *hm::stitching::RinkPointToRay(map->fit, {0, 0}));
    const auto generation = *hm::stitching::stitched_output_generation_id(map->hugin_generation, 0, 320, 240);
    for (auto* buffer : {fresh, missing}) {
      auto* frame = static_cast<NvDsFrameMeta*>(gst_buffer_get_nvds_batch_meta(buffer)->frame_meta_list->data);
      Check(hm::stitching::add_stitched_output_generation_meta(frame, generation), "rink generation attachment");
    }
    gpu::configure_rink_mini_map(sink, map);
    gpu::set_source_geometry(sink, 320, 240);
    g_object_set(sink, "show-player-tracking", FALSE, nullptr);
    for (const char* channel : {"program", "stitched"}) {
      g_object_set(sink, "channel", channel, nullptr);
      const auto rink = gpu::inspect_preview_overlays_for_test(sink, fresh);
      Check(
          rink.analytics_command_count > 80 && rink.path_count == 0,
          "Mini-map must draw independently of player box and analytics toggles");
      Check(
          rink.disc_colors.size() == 2 && !first.colors.empty() && rink.disc_colors.back() == first.colors.front(),
          "Mini-map dots must retain the detection box colour while box drawing is off");
      const auto stale = gpu::inspect_preview_overlays_for_test(sink, baked);
      Check(stale.analytics_command_count == 0, "Frame without calibration identity leaked mini-map");
    }
    g_object_set(sink, "channel", "program", nullptr);
    const auto original_print = g_set_print_handler(+[](const gchar* message) { rink_status_messages += message; });
    gpu::configure_rink_mini_map(sink, map);
    gpu::inspect_preview_overlays_for_test(sink, fresh);
    Check(rink_status_messages.find("status=ready") != std::string::npos, "Matching frame did not report readiness");
    g_usleep(G_USEC_PER_SEC);
    Check(
        gpu::inspect_preview_overlays_for_test(sink, missing).analytics_command_count == 0,
        "Mini-map without Program geometry must fail closed");
    Check(
        rink_status_messages.find(
            "status=unavailable message=The preview frame is missing its Program crop geometry") != std::string::npos,
        "Missing Program transform retained the previous ready status");
    auto* no_batch = gst_buffer_new();
    auto* list = gst_buffer_get_nvds_batch_meta(fresh)->frame_meta_list;
    auto* saved_frame = list->data;
    for (auto* no_metadata : {no_batch, fresh}) {
      if (no_metadata == fresh)
        list->data = nullptr;
      rink_status_messages.clear();
      gpu::configure_rink_mini_map(sink, map);
      Check(
          gpu::inspect_preview_overlays_for_test(sink, no_metadata).analytics_command_count == 0 &&
              rink_status_messages.find("status=unavailable message=The preview frame is missing its frame metadata") !=
                  std::string::npos,
          "Missing frame metadata did not explain suppressed mini-map drawing");
    }
    list->data = saved_frame;
    gst_buffer_unref(no_batch);
    g_set_print_handler(original_print);
    gpu::configure_rink_mini_map(sink, nullptr);
    Check(
        gpu::inspect_preview_overlays_for_test(sink, fresh).analytics_command_count == 0,
        "Disabling mini-map must clear its commands");
    gst_buffer_unref(missing);
    gst_buffer_unref(fresh);
    gst_buffer_unref(baked);
    gst_object_unref(sink);
    std::cout << "Actual preview collection: missing-transform fail-closed, baked suppression, "
                 "independent Stitched layers and disabled-command clearing passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
#endif
}
