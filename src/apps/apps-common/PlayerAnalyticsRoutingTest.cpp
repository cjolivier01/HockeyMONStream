#include "hstream/src/apps/apps-common/PlayerAnalyticsRouting.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"
#include "hstream/src/libs/common/TrackColorMeta.h"
#include "hstream/src/libs/player_analytics/Config.h"
#include "hstream/src/libs/player_analytics/FrameMeta.h"

#include <gstnvdsmeta.h>

#include <atomic>
#include <cstdlib>
#include <iostream>

namespace {
namespace pa = hm::player_analytics;
using Producer = hm::gst::ProgramTrackColorProducer;
constexpr uint64_t kId = (uint64_t{1} << 40) + 7;

void Require(bool value, const char* message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}

void Demand() {
  pa::Config config;
  config.drawing = {true, true, true};
  for (bool playtracker : {false, true}) {
    for (bool native : {false, true}) {
      Require(
          hm::gst::AddAnalyticsColorDemand(Producer::kNone, config.drawing_layers(), playtracker, native) ==
              Producer::kNone,
          "Drawing preferences alone allocated a color producer");
      config.pose.enabled = true;
      const auto producer =
          hm::gst::AddAnalyticsColorDemand(Producer::kNone, config.drawing_layers(), playtracker, native);
      Require(
          producer ==
              (playtracker  ? Producer::kPlayTracker
                   : native ? Producer::kNativeTracker
                            : Producer::kNone),
          "Semantic-only demand selected the wrong available producer");
      Require(
          hm::gst::AddAnalyticsColorDemand(Producer::kPlayTracker, config.drawing_layers(), playtracker, native) ==
              Producer::kPlayTracker,
          "Semantic demand replaced an existing Program box owner");
      config.pose.enabled = false;
    }
  }
}

struct Observation {
  bool saw_before_analytics{false};
  bool saw_after_analytics{false};
  bool expects_snapshot{false};
};

GstPadProbeReturn AddSemanticResult(GstPad*, GstPadProbeInfo* info, gpointer) {
  auto* batch = gst_buffer_get_nvds_batch_meta(GST_PAD_PROBE_INFO_BUFFER(info));
  Require(batch && batch->frame_meta_list, "Missing test batch before analytics");
  auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
  pa::FrameResult result;
  result.stream_id = frame->source_id;
  result.pts_ns = frame->buf_pts;
  result.coordinate_width = frame->source_frame_width;
  result.coordinate_height = frame->source_frame_height;
  result.player_count = 1;
  result.players[0].track_id = kId;
  result.players[0].box = {10, 20, 30, 60};
  result.players[0].has_pose = true;
  result.players[0].pose_observed_at = frame->buf_pts;
  result.players[0].pose[0] = {25, 25, .9F};
  Require(pa::AttachFrameResult(frame, result), "Could not attach synthetic semantic metadata at analytics output");
  return GST_PAD_PROBE_OK;
}

void RoutedMetadata(bool preview) {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* tracker = gst_element_factory_make("identity", "native_tracker");
  GstElement* analytics = gst_element_factory_make("identity", "player_analytics");
  GstElement* playtracker = gst_element_factory_make("identity", "playtracker_selection_only");
  Require(pipeline && tracker && analytics && playtracker, "Cannot create CPU identity graph");
  Require(
      hm::gst::SelectTrackedPreviewSource(playtracker, analytics, tracker) == playtracker &&
          hm::gst::SelectTrackedPreviewSource(nullptr, nullptr, tracker) == tracker &&
          !hm::gst::SelectTrackedPreviewSource(nullptr, nullptr, nullptr),
      "Tracked source preference order changed");
  gst_object_unref(playtracker);
  gst_bin_add_many(GST_BIN(pipeline), tracker, analytics, nullptr);
  Require(gst_element_link(tracker, analytics), "Cannot link tracker to analytics");

  GstPad* tracker_src = gst_element_get_static_pad(tracker, "src");
  GstPad* analytics_src = gst_element_get_static_pad(analytics, "src");
  gst_pad_add_probe(analytics_src, GST_PAD_PROBE_TYPE_BUFFER, AddSemanticResult, nullptr, nullptr);
  GstElement* selected = hm::gst::SelectTrackedPreviewSource(nullptr, analytics, tracker);
  Require(selected == analytics, "Tracker-only preview source is upstream of semantic results");
  std::atomic<unsigned> flags{preview ? 1U : 0U};
  Require(hm::preview_overlay::ConfigureTrackColorProducer(selected, true), "Cannot create semantic color owner");
  gpointer owner = g_object_get_data(G_OBJECT(selected), "hstream-track-color-producer");
  Require(
      owner && !g_object_get_data(G_OBJECT(tracker), "hstream-track-color-producer"),
      "Color owner is upstream of analytics or missing");
  Require(
      hm::preview_overlay::ConfigureTrackColorProducer(selected, false, &flags) &&
          owner == g_object_get_data(G_OBJECT(selected), "hstream-track-color-producer"),
      "Preview failed to reuse the existing encoded semantic color owner");

  Observation observation;
  observation.expects_snapshot = preview;
  gst_pad_add_probe(
      tracker_src,
      GST_PAD_PROBE_TYPE_BUFFER,
      +[](GstPad*, GstPadProbeInfo* info, gpointer data) {
        auto* batch = gst_buffer_get_nvds_batch_meta(GST_PAD_PROBE_INFO_BUFFER(info));
        auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
        Require(!pa::FindFrameResult(frame), "Semantic result unexpectedly exists before analytics");
        static_cast<Observation*>(data)->saw_before_analytics = true;
        return GST_PAD_PROBE_OK;
      },
      &observation,
      nullptr);
  GstPad* sink = gst_pad_new("sink", GST_PAD_SINK);
  gst_pad_set_element_private(sink, &observation);
  gst_pad_set_chain_function(
      sink, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
        auto* state = static_cast<Observation*>(gst_pad_get_element_private(pad));
        auto* batch = gst_buffer_get_nvds_batch_meta(buffer);
        auto* frame = static_cast<NvDsFrameMeta*>(batch->frame_meta_list->data);
        const auto* result = pa::FindFrameResult(frame);
        Require(
            result && result->player_count == 1 && result->players[0].track_id == kId &&
                result->players[0].color_slot != pa::kNoColor && result->players[0].has_pose,
            "Selected tracked preview missed analytics or shared semantic colors");
        auto* object = static_cast<NvDsObjectMeta*>(frame->obj_meta_list->data);
        const auto color = pa::kTrackPalette[result->players[0].color_slot];
        Require(
            object->rect_params.border_color.red == color.red &&
                object->rect_params.border_color.green == color.green &&
                object->rect_params.border_color.blue == color.blue,
            "Semantic layers and existing player boxes have different track colors");
        const auto* snapshot = hm::preview_overlay::find_overlay_snapshot_meta(frame);
        Require(
            static_cast<bool>(snapshot) == state->expects_snapshot,
            "Semantic drawing implicitly enabled preview boxes");
        if (snapshot)
          Require(snapshot->player_rects.size() == 1, "Preview snapshot missed the tracked player");
        state->saw_after_analytics = true;
        gst_buffer_unref(buffer);
        return GST_FLOW_OK;
      });
  Require(
      gst_pad_set_active(sink, TRUE) && gst_pad_link(analytics_src, sink) == GST_PAD_LINK_OK,
      "Cannot link metadata observation sink");
  Require(
      gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Identity graph failed start");
  gst_pad_push_event(tracker_src, gst_event_new_stream_start("analytics-routing"));
  GstCaps* caps = gst_caps_new_empty_simple("application/x-player-analytics-test");
  gst_pad_push_event(tracker_src, gst_event_new_caps(caps));
  gst_caps_unref(caps);
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  gst_pad_push_event(tracker_src, gst_event_new_segment(&segment));

  auto* batch = nvds_create_batch_meta(1);
  auto* frame = nvds_acquire_frame_meta_from_pool(batch);
  frame->source_id = 4;
  frame->source_frame_width = 640;
  frame->source_frame_height = 360;
  frame->buf_pts = pa::kSecond;
  frame->frame_num = 30;
  nvds_add_frame_meta_to_batch(batch, frame);
  auto* object = nvds_acquire_obj_meta_from_pool(batch);
  object->object_id = kId;
  object->class_id = 0;
  object->rect_params.left = 10;
  object->rect_params.top = 20;
  object->rect_params.width = 30;
  object->rect_params.height = 60;
  nvds_add_obj_meta_to_frame(frame, object, nullptr);
  GstBuffer* buffer = gst_buffer_new();
  auto* meta =
      gst_buffer_add_nvds_meta(buffer, batch, nullptr, nvds_batch_meta_copy_func, nvds_batch_meta_release_func);
  Require(meta != nullptr, "Cannot attach test batch");
  meta->meta_type = NVDS_BATCH_GST_META;
  Require(
      gst_pad_push(tracker_src, buffer) == GST_FLOW_OK && observation.saw_before_analytics &&
          observation.saw_after_analytics,
      "Metadata failed to flow from tracker through analytics to selected preview source");
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_pad_unlink(analytics_src, sink);
  gst_pad_set_active(sink, FALSE);
  gst_object_unref(sink);
  gst_object_unref(analytics_src);
  gst_object_unref(tracker_src);
  gst_object_unref(pipeline);
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  Demand();
  RoutedMetadata(false);
  RoutedMetadata(true);
  std::cout << "Compute-gated color demand, post-analytics tracker-only routing and one shared color owner passed\n";
}
