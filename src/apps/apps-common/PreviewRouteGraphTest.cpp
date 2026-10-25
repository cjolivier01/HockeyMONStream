#include "hstream/src/apps/apps-common/PreviewRouteGraph.h"

#include <cstdlib>
#include <iostream>

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
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  existing_tee();
  tracked();
  source();
  bridge();
  return 0;
}
