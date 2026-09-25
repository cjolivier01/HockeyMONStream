#include "hstream/src/apps/apps-common/HmGpuPreview.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <gstnvdsmeta.h>

#include <cstring>
#include <iostream>
#include <stdexcept>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {
namespace pa = hm::player_analytics;
namespace preview = hm::preview_overlay;
namespace gpu = hm::gpu_preview;

void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

// Real collect_preview_overlays caller, metadata only: no GL context, video
// readback, compositor substitution or reproduction of its selection algorithm.
GstBuffer* Buffer(bool transform, uint32_t baked) {
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
  nvds_add_obj_meta_to_frame(frame, object, nullptr);
  Check(preview::add_overlay_snapshot_meta(frame), "snapshot attachment failed");
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
