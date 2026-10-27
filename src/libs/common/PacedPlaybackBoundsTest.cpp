#include "hstream/src/libs/common/PacedPlayback.h"

#include <gst/base/gstbasesink.h>

#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void Check(bool value, const char* detail) {
  if (!value)
    throw std::runtime_error(detail);
}

struct Fixture {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* ingress = gst_element_factory_make("identity", nullptr);
  GstElement* source_sink = gst_element_factory_make("fakesink", nullptr);
  GstElement* route = gst_element_factory_make("identity", nullptr);
  GstElement* sink = gst_element_factory_make("fakesink", nullptr);
  std::shared_ptr<hm::gst::PacedPlayback> owner;

  Fixture() {
    Check(pipeline && ingress && source_sink && route && sink, "Missing test elements");
    g_object_set(source_sink, "sync", TRUE, "async", FALSE, nullptr);
    g_object_set(sink, "sync", TRUE, "async", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline), ingress, source_sink, route, sink, nullptr);
    Check(gst_element_link(ingress, source_sink) && gst_element_link(route, sink), "Could not link test branches");
    auto* pad = gst_element_get_static_pad(ingress, "src");
    owner = hm::gst::PacedPlayback::Create(pipeline, pad, {16683, 4170, 0, 2, GST_SECOND, 1, 16384, 2, 3 * GST_SECOND});
    gst_object_unref(pad);
    Check(owner && owner->AttachVideoSink(sink, 2), "Could not attach bounded presentation owner");
    Check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Could not start test graph");
    for (auto* input : {ingress, route}) {
      auto* pad = gst_element_get_static_pad(input, "sink");
      gst_pad_send_event(pad, gst_event_new_stream_start(input == ingress ? "source" : "route"));
      auto* caps = gst_caps_from_string("video/x-raw,format=RGBA,width=1,height=1,framerate=0/1");
      gst_pad_send_event(pad, gst_event_new_caps(caps));
      gst_caps_unref(caps);
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      gst_pad_send_event(pad, gst_event_new_segment(&segment));
      gst_object_unref(pad);
    }
  }
  ~Fixture() {
    if (owner)
      owner->Cancel();
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
  void Push(GstElement* input, guint frame, GstClockTime duration = 10 * GST_MSECOND) {
    auto* pad = gst_element_get_static_pad(input, "sink");
    auto* buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(buffer) = frame * 10 * GST_MSECOND;
    GST_BUFFER_DURATION(buffer) = duration;
    const auto result = gst_pad_chain(pad, buffer);
    gst_object_unref(pad);
    Check(result == GST_FLOW_OK, "Test input did not reach its pad probe");
  }
  void Error(const char* expected) {
    auto* bus = gst_element_get_bus(pipeline);
    auto* message = gst_bus_timed_pop_filtered(bus, GST_SECOND, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    Check(message, "Invalid live presentation did not report a stream error");
    GError* error{};
    gchar* debug{};
    gst_message_parse_error(message, &error, &debug);
    const bool matched = debug && std::string(debug).find(expected) != std::string::npos;
    g_clear_error(&error);
    g_free(debug);
    gst_message_unref(message);
    Check(matched, "Presentation failed for an unexpected reason");
  }
};
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    {
      Fixture test;
      for (guint frame = 0; frame < 4; ++frame)
        test.Push(test.ingress, frame);
      test.Push(test.route, 0);
      test.Error("bounded history");
    }
    {
      Fixture test;
      test.Push(test.ingress, 0);
      Check(gst_element_send_event(test.sink, gst_event_new_latency(100 * GST_MSECOND)), "Device lead event rejected");
      Check(gst_base_sink_get_latency(GST_BASE_SINK(test.sink)) == 100 * GST_MSECOND, "Device lead was not negotiated");
      test.Push(test.route, 0);
      test.Error("bounded GPU retention");
    }
    {
      Fixture test;
      test.Push(test.ingress, 0, GST_CLOCK_TIME_NONE);
      test.Error("finite source intervals");
    }
    {
      Fixture test;
      auto* pad = gst_element_get_static_pad(test.ingress, "sink");
      for (const char* description :
           {"video/x-raw,format=RGBA,width=1,height=1,framerate=30/1",
            "video/x-raw,format=RGBA,width=1,height=1,framerate=0/1"}) {
        auto* caps = gst_caps_from_string(description);
        Check(gst_pad_send_event(pad, gst_event_new_caps(caps)), "Test CAPS rejected");
        gst_caps_unref(caps);
      }
      gst_object_unref(pad);
      test.Push(test.ingress, 0, GST_CLOCK_TIME_NONE);
      test.Error("finite source intervals");
    }
    {
      Fixture test;
      test.Push(test.ingress, 0, guint64(INT64_MAX) + 1);
      test.Error("finite source intervals");
    }
    {
      Fixture test;
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      test.Push(test.ingress, 0);
      std::atomic<bool> retired{false};
      auto waiting = std::async(std::launch::async, [&] {
        GstClockTime presentation;
        return test.owner->PresentationTime(segment, GST_SECOND, &presentation, &retired);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      Check(
          waiting.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout,
          "An unadmitted route must wait for its shared source interval");
      Check(
          gst_element_set_state(test.pipeline, GST_STATE_PAUSED) != GST_STATE_CHANGE_FAILURE,
          "Could not pause shared mapping fixture");
      retired.store(true, std::memory_order_release);
      test.owner->WakePresentationWaiters();
      Check(
          waiting.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready &&
              waiting.get() == GST_FLOW_FLUSHING,
          "Retiring one paused route must promptly release only its mapping wait");
      GstClockTime presentation;
      Check(
          test.owner->PresentationTime(segment, 0, &presentation) == GST_FLOW_OK,
          "Retiring a preview route must retain the shared mapping history and epoch");
    }
    {
      Fixture test;
      GstSegment segment;
      gst_segment_init(&segment, GST_FORMAT_TIME);
      auto waiting = std::async(std::launch::async, [&] {
        GstClockTime presentation;
        return test.owner->PresentationTime(segment, GST_SECOND, &presentation);
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      Check(
          waiting.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout,
          "Unadmitted transport video must wait for its shared source interval");
      test.owner->Cancel();
      Check(
          waiting.wait_for(std::chrono::milliseconds(100)) == std::future_status::ready &&
              waiting.get() == GST_FLOW_FLUSHING,
          "Cancellation must retire transport mapping waits promptly");
    }
    std::cout << "History eviction, device lead and unknown/overflowing source duration fail explicitly\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
