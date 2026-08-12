#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/RuntimeOutputCaps.h"

#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>

namespace {

struct RuntimeSizedTransform {
  GstBaseTransform parent;
  GstCaps* runtime_caps;
};

struct RuntimeSizedTransformClass {
  GstBaseTransformClass parent_class;
};

G_DEFINE_TYPE(RuntimeSizedTransform, runtime_sized_transform, GST_TYPE_BASE_TRANSFORM)

GstCaps* runtime_sized_transform_caps(
    GstBaseTransform* transform,
    GstPadDirection direction,
    GstCaps* caps,
    GstCaps* filter) {
  auto* runtime_transform = reinterpret_cast<RuntimeSizedTransform*>(transform);
  GstCaps* result = direction == GST_PAD_SINK && runtime_transform->runtime_caps
      ? gst_caps_ref(runtime_transform->runtime_caps)
      : gst_caps_ref(caps);
  if (filter) {
    GstCaps* intersection = gst_caps_intersect_full(filter, result, GST_CAPS_INTERSECT_FIRST);
    gst_caps_unref(result);
    result = intersection;
  }
  return result;
}

GstFlowReturn runtime_sized_transform_in_place(GstBaseTransform*, GstBuffer*) {
  return GST_FLOW_OK;
}

void runtime_sized_transform_finalize(GObject* object) {
  auto* transform = reinterpret_cast<RuntimeSizedTransform*>(object);
  if (transform->runtime_caps) {
    gst_caps_unref(transform->runtime_caps);
  }
  G_OBJECT_CLASS(runtime_sized_transform_parent_class)->finalize(object);
}

void runtime_sized_transform_class_init(RuntimeSizedTransformClass* klass) {
  static GstStaticPadTemplate sink_template =
      GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
  static GstStaticPadTemplate src_template =
      GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
  GstElementClass* element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_add_static_pad_template(element_class, &sink_template);
  gst_element_class_add_static_pad_template(element_class, &src_template);
  gst_element_class_set_static_metadata(
      element_class,
      "Runtime-sized test transform",
      "Filter/Video",
      "Models a transform whose output size becomes known at runtime",
      "HStream tests");
  GstBaseTransformClass* transform_class = GST_BASE_TRANSFORM_CLASS(klass);
  transform_class->transform_caps = runtime_sized_transform_caps;
  transform_class->transform_ip = runtime_sized_transform_in_place;
  G_OBJECT_CLASS(klass)->finalize = runtime_sized_transform_finalize;
}

void runtime_sized_transform_init(RuntimeSizedTransform* transform) {
  transform->runtime_caps = nullptr;
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(transform), TRUE);
}

void set_runtime_transform_caps(RuntimeSizedTransform* transform, GstCaps* caps) {
  if (transform->runtime_caps) {
    gst_caps_unref(transform->runtime_caps);
  }
  transform->runtime_caps = gst_caps_ref(caps);
}

struct DownstreamObserver {
  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::pair<int, int>> caps_dimensions;
  unsigned int buffer_count{0};
};

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
  }
  return condition;
}

bool caps_dimensions(GstCaps* caps, int* width, int* height) {
  if (!caps || gst_caps_is_empty(caps) || gst_caps_is_any(caps)) {
    return false;
  }
  const GstStructure* structure = gst_caps_get_structure(caps, 0);
  return gst_structure_get_int(structure, "width", width) && gst_structure_get_int(structure, "height", height);
}

bool pad_has_dimensions(GstPad* pad, int expected_width, int expected_height) {
  GstCaps* caps = gst_pad_get_current_caps(pad);
  int width = 0;
  int height = 0;
  const bool matches = caps_dimensions(caps, &width, &height) && width == expected_width && height == expected_height;
  if (caps) {
    gst_caps_unref(caps);
  }
  return matches;
}

bool pad_sticky_caps_have_dimensions(GstPad* pad, int expected_width, int expected_height) {
  GstEvent* event = gst_pad_get_sticky_event(pad, GST_EVENT_CAPS, 0);
  if (!event) {
    return false;
  }
  GstCaps* caps = nullptr;
  gst_event_parse_caps(event, &caps);
  int width = 0;
  int height = 0;
  const bool matches = caps_dimensions(caps, &width, &height) && width == expected_width && height == expected_height;
  gst_event_unref(event);
  return matches;
}

GstPadProbeReturn observe_downstream(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
  auto* observer = static_cast<DownstreamObserver*>(user_data);
  std::lock_guard<std::mutex> lock(observer->mutex);
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
    GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS) {
      GstCaps* caps = nullptr;
      gst_event_parse_caps(event, &caps);
      int width = 0;
      int height = 0;
      if (caps_dimensions(caps, &width, &height)) {
        observer->caps_dimensions.emplace_back(width, height);
      }
    }
  }
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
    ++observer->buffer_count;
  }
  observer->condition.notify_all();
  return GST_PAD_PROBE_OK;
}

bool wait_for_buffers(DownstreamObserver* observer, unsigned int expected_count) {
  std::unique_lock<std::mutex> lock(observer->mutex);
  return observer->condition.wait_for(
      lock, std::chrono::seconds(2), [&] { return observer->buffer_count >= expected_count; });
}

bool observed_caps(DownstreamObserver* observer, int width, int height) {
  std::lock_guard<std::mutex> lock(observer->mutex);
  return std::find(observer->caps_dimensions.begin(), observer->caps_dimensions.end(), std::pair(width, height)) !=
      observer->caps_dimensions.end();
}

bool push_frame(GstElement* source, int width, int height, GstClockTime timestamp) {
  GstBuffer* buffer = gst_buffer_new_allocate(nullptr, static_cast<gsize>(width) * height * 3, nullptr);
  if (!buffer) {
    return false;
  }
  GST_BUFFER_PTS(buffer) = timestamp;
  GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
  GstFlowReturn flow = GST_FLOW_ERROR;
  g_signal_emit_by_name(source, "push-buffer", buffer, &flow);
  gst_buffer_unref(buffer);
  return flow == GST_FLOW_OK;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = true;

  GstElement* pipeline = gst_pipeline_new("runtime-output-caps-test");
  GstElement* source = gst_element_factory_make("appsrc", "source");
  GstElement* transform =
      GST_ELEMENT(g_object_new(runtime_sized_transform_get_type(), "name", "runtime-sized-transform", nullptr));
  GstElement* sink = gst_element_factory_make("fakesink", "downstream-sink");
  if (!pipeline || !source || !transform || !sink) {
    if (pipeline) {
      gst_object_unref(pipeline);
    }
    std::cerr << "FAIL: required GStreamer elements are unavailable\n";
    return 1;
  }

  constexpr int kInitialWidth = 320;
  constexpr int kInitialHeight = 180;
  constexpr int kRuntimeWidth = 777;
  constexpr int kRuntimeHeight = 333;
  GstCaps* initial_caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "RGB",
      "width",
      G_TYPE_INT,
      kInitialWidth,
      "height",
      G_TYPE_INT,
      kInitialHeight,
      "framerate",
      GST_TYPE_FRACTION,
      30,
      1,
      nullptr);
  g_object_set(source, "caps", initial_caps, "format", GST_FORMAT_TIME, "is-live", TRUE, nullptr);
  g_object_set(sink, "async", FALSE, "sync", FALSE, nullptr);
  gst_caps_unref(initial_caps);

  gst_bin_add_many(GST_BIN(pipeline), source, transform, sink, nullptr);
  ok &= expect(gst_element_link_many(source, transform, sink, nullptr), "test pipeline must link");

  DownstreamObserver observer;
  GstPad* transform_src_pad = gst_element_get_static_pad(transform, "src");
  GstPad* sink_pad = gst_element_get_static_pad(sink, "sink");
  const gulong probe_id = gst_pad_add_probe(
      sink_pad,
      static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM | GST_PAD_PROBE_TYPE_BUFFER),
      observe_downstream,
      &observer,
      nullptr);

  ok &= expect(
      gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
      "test pipeline must enter PLAYING");
  ok &= expect(push_frame(source, kInitialWidth, kInitialHeight, 0), "initial fixed-caps buffer must be accepted");
  ok &= expect(wait_for_buffers(&observer, 1), "initial fixed-caps buffer must reach downstream");
  ok &= expect(
      pad_has_dimensions(transform_src_pad, kInitialWidth, kInitialHeight),
      "base transform must initially negotiate the fixed input dimensions");
  ok &=
      expect(observed_caps(&observer, kInitialWidth, kInitialHeight), "downstream must observe the initial fixed caps");

  GstCaps* runtime_caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "RGB",
      "width",
      G_TYPE_INT,
      kRuntimeWidth,
      "height",
      G_TYPE_INT,
      kRuntimeHeight,
      "framerate",
      GST_TYPE_FRACTION,
      30,
      1,
      nullptr);
  set_runtime_transform_caps(reinterpret_cast<RuntimeSizedTransform*>(transform), runtime_caps);
  ok &= expect(
      hm::videoprep::update_runtime_output_caps(GST_BASE_TRANSFORM(transform), runtime_caps) ==
          hm::videoprep::RuntimeOutputCapsUpdateResult::kUpdated,
      "runtime output caps update must succeed");

  ok &= expect(
      pad_has_dimensions(transform_src_pad, kRuntimeWidth, kRuntimeHeight),
      "GstBaseTransform src current caps must reflect runtime dimensions");
  ok &= expect(
      pad_sticky_caps_have_dimensions(transform_src_pad, kRuntimeWidth, kRuntimeHeight),
      "GstBaseTransform src sticky caps must reflect runtime dimensions");
  ok &= expect(
      pad_has_dimensions(sink_pad, kRuntimeWidth, kRuntimeHeight),
      "downstream peer current caps must reflect runtime dimensions");
  ok &= expect(
      pad_sticky_caps_have_dimensions(sink_pad, kRuntimeWidth, kRuntimeHeight),
      "downstream peer sticky caps must reflect runtime dimensions");
  ok &= expect(
      observed_caps(&observer, kRuntimeWidth, kRuntimeHeight),
      "downstream caps observer must receive the runtime dimensions");

  ok &=
      expect(push_frame(source, kRuntimeWidth, kRuntimeHeight, GST_SECOND / 30), "post-update buffer must be accepted");
  ok &= expect(wait_for_buffers(&observer, 2), "post-update buffer must continue flowing downstream");
  ok &= expect(
      pad_has_dimensions(transform_src_pad, kRuntimeWidth, kRuntimeHeight) &&
          pad_has_dimensions(sink_pad, kRuntimeWidth, kRuntimeHeight),
      "post-update buffer flow must preserve runtime caps on transform and downstream");
  ok &= expect(
      hm::videoprep::update_runtime_output_caps(nullptr, nullptr) ==
          hm::videoprep::RuntimeOutputCapsUpdateResult::kFailed,
      "invalid runtime caps update arguments must fail safely");

  gst_pad_unlink(transform_src_pad, sink_pad);
  ok &= expect(
      hm::videoprep::update_runtime_output_caps(GST_BASE_TRANSFORM(transform), runtime_caps) ==
          hm::videoprep::RuntimeOutputCapsUpdateResult::kCancelled,
      "runtime caps update on an unlinked downstream pad must be cancellation");
  gst_caps_unref(runtime_caps);

  GstFlowReturn eos_flow = GST_FLOW_ERROR;
  g_signal_emit_by_name(source, "end-of-stream", &eos_flow);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_pad_remove_probe(sink_pad, probe_id);
  gst_object_unref(sink_pad);
  gst_object_unref(transform_src_pad);
  gst_object_unref(pipeline);
  return ok ? 0 : 1;
}
