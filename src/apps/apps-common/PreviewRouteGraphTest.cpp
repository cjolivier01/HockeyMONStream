#include "hstream/src/apps/apps-common/PreviewRouteGraph.h"

#include <gst/app/gstappsrc.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {
using namespace hm::preview;
void check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}
GstElement* element(const char* type, const char* name) {
  auto* result = gst_element_factory_make(type, name);
  check(result, "Missing test element");
  if (std::string(type) == "fakesink")
    g_object_set(result, "sync", FALSE, "async", FALSE, nullptr);
  return result;
}
GstPad* request(GstElement* tee, GstElement* destination) {
  auto* type = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(tee), "src_%u");
  auto* result = gst_element_request_pad(tee, type, nullptr, nullptr);
  auto* sink = gst_element_get_static_pad(destination, "sink");
  check(result && sink && gst_pad_link(result, sink) == GST_PAD_LINK_OK, "Test tee link failed");
  gst_object_unref(sink);
  return result;
}
bool linked(GstElement* source, GstElement* destination) {
  auto* out = gst_element_get_static_pad(source, "src");
  auto* peer = out ? gst_pad_get_peer(out) : nullptr;
  const bool result = peer && GST_OBJECT_PARENT(peer) == GST_OBJECT(destination);
  if (peer)
    gst_object_unref(peer);
  if (out)
    gst_object_unref(out);
  return result;
}
unsigned pads(GstElement* object, bool source) {
  auto* iterator = source ? gst_element_iterate_src_pads(object) : gst_element_iterate_sink_pads(object);
  GValue value = G_VALUE_INIT;
  unsigned count = 0;
  while (gst_iterator_next(iterator, &value) == GST_ITERATOR_OK) {
    ++count;
    g_value_reset(&value);
  }
  if (G_VALUE_TYPE(&value))
    g_value_unset(&value);
  gst_iterator_free(iterator);
  return count;
}
bool absent(GstElement* parent, const char* name) {
  auto* value = gst_bin_get_by_name(GST_BIN(parent), name);
  if (value)
    gst_object_unref(value);
  return value == nullptr;
}
std::vector<GstElement*> path(GstElement* queue = nullptr, bool terminal = true) {
  std::vector<GstElement*> result{
      element("identity", "ingress"), queue ? queue : element("queue", "preview-queue"), element("identity", "drain")};
  if (terminal)
    result.push_back(element("fakesink", "preview-sink"));
  return result;
}
void existing_tee() {
  auto* pipeline = gst_pipeline_new("program");
  auto* tee = element("tee", "output-tee");
  auto* archive = element("fakesink", "archive");
  gst_bin_add_many(GST_BIN(pipeline), tee, archive, nullptr);
  auto* archive_pad = request(tee, archive);
  std::string error;
  auto attach = RouteAttachment::Create({RouteTap::Kind::kExistingTee, pipeline, tee}, path(), nullptr, &error);
  check(attach && pads(tee, true) == 2, "Program did not reuse existing tee");
  check(!attach->Commit([] { return false; }), "Rejected publication committed");
  attach.reset();
  check(
      pads(tee, true) == 1 && gst_pad_is_linked(archive_pad) && absent(pipeline, "ingress"),
      "Program rollback disturbed archive or retained preview request/nodes");
  attach = RouteAttachment::Create({RouteTap::Kind::kExistingTee, pipeline, tee}, path(), nullptr, &error);
  check(attach && attach->Commit([] { return true; }), "Program commit failed");
  attach.reset();
  check(
      pads(tee, true) == 2 && gst_pad_is_linked(archive_pad) && !absent(pipeline, "preview-sink"),
      "Program commit lost original archive or preview");
  gst_object_unref(archive_pad);
  gst_object_unref(pipeline);
}
void tracked() {
  auto* pipeline = gst_pipeline_new("tracked");
  auto* producer = element("identity", "tracker");
  auto* downstream = element("fakesink", "cropper");
  gst_bin_add_many(GST_BIN(pipeline), producer, downstream, nullptr);
  check(gst_element_link(producer, downstream), "Original tracked link failed");
  RouteTap tap{RouteTap::Kind::kAfterElement, pipeline, producer, nullptr, nullptr, "tracked-preview-tee"};
  std::string error;
  auto broken = path();
  broken.push_back(element("identity", "unreachable")); // A sink has no src pad.
  check(!RouteAttachment::Create(tap, broken, nullptr, &error), "Invalid consumer path was attached");
  check(
      linked(producer, downstream) && absent(pipeline, "tracked-preview-tee") && absent(pipeline, "ingress"),
      "Failed tracked attachment did not restore processing order");
  auto attach = RouteAttachment::Create(tap, path(), nullptr, &error);
  check(attach && !attach->Commit([] { return false; }), "Tracked rejected publication committed");
  attach.reset();
  check(linked(producer, downstream) && absent(pipeline, "tracked-preview-tee"), "Tracked rollback failed");
  attach = RouteAttachment::Create(tap, path(), nullptr, &error);
  check(attach && attach->Commit([] { return true; }), "Tracked commit failed");
  attach.reset();
  auto* tee = gst_bin_get_by_name(GST_BIN(pipeline), "tracked-preview-tee");
  check(tee && linked(producer, tee) && pads(tee, true) == 2, "Tracked tap moved before its producer");
  gst_object_unref(tee);
  gst_object_unref(pipeline);
}
void source() {
  auto* pipeline = gst_pipeline_new("source");
  auto* tee = element("tee", "source-tee");
  auto* queue = element("queue", "fakequeue");
  auto* drain = element("fakesink", "old-drain");
  gst_bin_add_many(GST_BIN(pipeline), tee, queue, drain, nullptr);
  check(gst_element_link(queue, drain), "Original source drain link failed");
  auto* original = request(tee, queue);
  g_object_set(queue, "max-size-buffers", 47U, nullptr);
  RouteTap tap{RouteTap::Kind::kReplaceDrain, pipeline, tee, queue, drain};
  std::string error;
  auto attach = RouteAttachment::Create(tap, path(queue), nullptr, &error);
  check(attach && pads(tee, true) == 1, "Source allocated another tee request pad");
  g_object_set(queue, "max-size-buffers", 1U, nullptr);
  check(!attach->Commit([] { return false; }), "Source rejected publication committed");
  attach.reset();
  guint capacity = 0;
  g_object_get(queue, "max-size-buffers", &capacity, nullptr);
  auto* queue_pad = gst_element_get_static_pad(queue, "sink");
  auto* peer = gst_pad_get_peer(queue_pad);
  check(
      peer == original && linked(queue, drain) && capacity == 47 && absent(pipeline, "preview-sink"),
      "Source rollback lost original tee pad, queue settings or drain");
  gst_object_unref(peer);
  gst_object_unref(queue_pad);
  attach = RouteAttachment::Create(tap, path(queue), nullptr, &error);
  check(attach && attach->Commit([] { return true; }), "Source commit failed");
  attach.reset();
  check(
      absent(pipeline, "old-drain") && pads(tee, true) == 1 && !absent(pipeline, "preview-sink"),
      "Source commit retained obsolete drain or duplicated a source branch");
  gst_object_unref(original);
  gst_object_unref(pipeline);
}
void bridge() {
  auto* pipeline = gst_pipeline_new("bridge");
  auto* sources = gst_bin_new("sources");
  auto* camera = gst_bin_new("camera");
  auto* target = gst_bin_new("target");
  auto* tee = element("tee", "camera-tee");
  auto* sink = element("fakesink", "browser-consumer");
  gst_bin_add(GST_BIN(camera), tee);
  gst_bin_add(GST_BIN(sources), camera);
  gst_bin_add(GST_BIN(target), sink);
  auto* actual = gst_element_get_static_pad(sink, "sink");
  auto* input = gst_ghost_pad_new("route", actual);
  check(gst_element_add_pad(target, input), "Could not expose shared target input");
  gst_object_unref(actual);
  gst_bin_add_many(GST_BIN(pipeline), sources, target, nullptr);
  std::string error;
  auto attach =
      RouteAttachment::Create({RouteTap::Kind::kExistingTee, camera, tee}, path(nullptr, false), input, &error);
  check(
      attach && pads(camera, true) == 1 && pads(sources, true) == 1 && gst_pad_is_linked(input),
      "Shared target was not bridged through exact source parents");
  attach.reset();
  check(
      pads(camera, true) == 0 && pads(sources, true) == 0 && !gst_pad_is_linked(input) && pads(tee, true) == 0,
      "Rollback retained cross-bin ghost or request pads");
  attach = RouteAttachment::Create({RouteTap::Kind::kExistingTee, camera, tee}, path(nullptr, false), input, &error);
  check(attach && attach->Commit([] { return true; }), "Shared target commit failed");
  attach.reset();
  check(
      gst_pad_is_linked(input) && pads(camera, true) == 1 && pads(sources, true) == 1,
      "Commit removed required source bridge");
  gst_object_unref(pipeline);
}
void moved_source_eos() {
  auto* pipeline = gst_pipeline_new("moved-source-eos");
  auto* sources = gst_bin_new("sources");
  auto* camera = gst_bin_new("camera");
  auto* producer = element("fakesrc", "producer");
  auto* tee = element("tee", "camera-tee");
  auto* queue = element("queue", "source-queue");
  auto* old_sink = element("fakesink", "old-source-drain");
  auto* consumer = element("fakesink", "shared-consumer");
  g_object_set(producer, "num-buffers", 3, nullptr);
  gst_bin_add_many(GST_BIN(camera), producer, tee, queue, old_sink, nullptr);
  check(gst_element_link(producer, tee) && gst_element_link(queue, old_sink), "Source fixture link failed");
  auto* requested = request(tee, queue);
  gst_bin_add(GST_BIN(sources), camera);
  gst_bin_add_many(GST_BIN(pipeline), sources, consumer, nullptr);
  check(GST_OBJECT_FLAG_IS_SET(sources, GST_ELEMENT_FLAG_SINK), "Fixture has no inherited source drain");
  auto* input = gst_element_get_static_pad(consumer, "sink");
  std::string error;
  auto attach = RouteAttachment::Create(
      {RouteTap::Kind::kReplaceDrain, camera, tee, queue, old_sink}, path(queue, false), input, &error);
  check(attach && attach->Commit([] { return true; }), "Shared source replacement failed");
  attach.reset();
  check(!GST_OBJECT_FLAG_IS_SET(sources, GST_ELEMENT_FLAG_SINK), "Source ancestors retain a phantom EOS sink");
  check(GST_OBJECT_FLAG_IS_SET(pipeline, GST_ELEMENT_FLAG_SINK), "Shared consumer lost EOS ownership");
  auto* bus = gst_element_get_bus(pipeline);
  check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "EOS fixture failed start");
  auto* message = gst_bus_timed_pop_filtered(bus, GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "Shared source replacement prevented pipeline EOS");
  gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus);
  gst_object_unref(input);
  gst_object_unref(requested);
  gst_object_unref(pipeline);
}

void playback_clock(bool stop_while_waiting) {
  // Nonzero recording timestamps must follow their segment, rather than wait
  // for the recording's absolute offset. The other output models a disconnected
  // browser: it never controls pacing or receives a command from this test.
  auto* pipeline = gst_pipeline_new("paced-program");
  auto* source = element("appsrc", "recording");
  auto* tee = element("tee", "program");
  auto* browser_queue = element("queue", "browser-queue");
  auto* browser = element("fakesink", "disconnected-browser");
  g_object_set(source, "format", GST_FORMAT_TIME, "handle-segment-change", TRUE, nullptr);
  gst_bin_add_many(GST_BIN(pipeline), source, tee, browser_queue, browser, nullptr);
  check(gst_element_link(source, tee) && gst_element_link(browser_queue, browser), "Clock fixture source link failed");
  auto* browser_pad = request(tee, browser_queue);
  std::string error;
  check(AttachPlaybackClock({RouteTap::Kind::kExistingTee, pipeline, tee}, &error), "Program clock attachment failed");
  auto* sink = gst_bin_get_by_name(GST_BIN(pipeline), "playback_clock");
  std::atomic<unsigned> frames{0};
  g_object_set(sink, "signal-handoffs", TRUE, nullptr);
  g_signal_connect(
      sink,
      "handoff",
      G_CALLBACK(+[](GstElement*, GstBuffer* buffer, GstPad*, gpointer data) {
        auto& frames = *static_cast<std::atomic<unsigned>*>(data);
        const auto index = frames.fetch_add(1);
        check(
            GST_BUFFER_PTS(buffer) == GstClockTime{60 * GST_SECOND} + index * 100 * GST_MSECOND,
            "Clock changed source timestamps");
        // Briefly fall behind. A pacing sink must retain these late frames.
        if (index == 3)
          std::this_thread::sleep_for(std::chrono::milliseconds(250));
      }),
      &frames);
  auto* caps = gst_caps_new_empty_simple("application/x-clock-test");
  GstSegment segment;
  gst_segment_init(&segment, GST_FORMAT_TIME);
  segment.start = segment.time = 60 * GST_SECOND;
  const auto started = std::chrono::steady_clock::now();
  check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "Clock fixture failed start");
  const unsigned count = stop_while_waiting ? 2 : 11;
  for (unsigned index = 0; index < count; ++index) {
    auto* buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(buffer) =
        segment.start + (stop_while_waiting && index ? 3600 * GST_SECOND : index * 100 * GST_MSECOND);
    GST_BUFFER_DURATION(buffer) = 100 * GST_MSECOND;
    auto* sample = gst_sample_new(buffer, caps, &segment, nullptr);
    check(gst_app_src_push_sample(GST_APP_SRC(source), sample) == GST_FLOW_OK, "Clock fixture push failed");
    gst_sample_unref(sample);
    gst_buffer_unref(buffer);
  }
  gst_caps_unref(caps);
  auto* bus = gst_element_get_bus(pipeline);
  if (stop_while_waiting) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto stopping = std::chrono::steady_clock::now();
    check(gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_FAILURE, "Clock fixture failed stop");
    check(
        std::chrono::steady_clock::now() - stopping < std::chrono::seconds(1),
        "Stop waited on future video timestamps");
  } else {
    check(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK, "Clock fixture EOS failed");
    auto* message =
        gst_bus_timed_pop_filtered(bus, 3 * GST_SECOND, GstMessageType(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    check(message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS, "Clock fixture did not complete without a browser");
    gst_message_unref(message);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    check(
        elapsed >= std::chrono::milliseconds(950) && elapsed < std::chrono::seconds(3),
        "Program did not follow segment playback time");
    check(frames == count, "Clock dropped late video frames");
    gst_element_set_state(pipeline, GST_STATE_NULL);
  }
  gst_object_unref(sink);
  gst_object_unref(bus);
  gst_object_unref(browser_pad);
  gst_object_unref(pipeline);
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  existing_tee();
  tracked();
  source();
  bridge();
  moved_source_eos();
  playback_clock(false);
  playback_clock(true);
  return 0;
}
