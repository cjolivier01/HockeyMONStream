#include "AsyncInferenceSeekRecovery.h"

#include <gst/base/gstbasetransform.h>
#include <cstdio>

namespace {

struct RecoveryTransform {
  GstBaseTransform parent;
  GstBuffer* pending;
  GstFlowReturn cached;
  gboolean flush_during_submit;
  gboolean immediate;
  guint submissions;
  guint barriers;
};
struct RecoveryTransformClass {
  GstBaseTransformClass parent;
};

G_DEFINE_TYPE(RecoveryTransform, recovery_transform, GST_TYPE_BASE_TRANSFORM)

GstFlowReturn submit(GstBaseTransform* base, gboolean, GstBuffer* buffer) {
  auto* self = reinterpret_cast<RecoveryTransform*>(base);
  ++self->submissions;
  if (self->immediate) {
    self->cached = gst_pad_push(base->srcpad, buffer);
  } else {
    self->pending = buffer;
  }
  if (self->flush_during_submit)
    gst_pad_push_event(base->srcpad, gst_event_new_flush_start());
  return GST_FLOW_OK;
}

GstFlowReturn generate(GstBaseTransform* base, GstBuffer**) {
  return reinterpret_cast<RecoveryTransform*>(base)->cached;
}

gboolean event(GstBaseTransform* base, GstEvent* event) {
  auto* self = reinterpret_cast<RecoveryTransform*>(base);
  if (GST_EVENT_TYPE(event) == GST_EVENT_CUSTOM_DOWNSTREAM) {
    ++self->barriers;
    // Model nvinfer's serialized marker: the queued input finishes before
    // forwarding the event, replacing the previous epoch's cached result.
    if (self->pending) {
      GstBuffer* buffer = self->pending;
      self->pending = nullptr;
      self->cached = gst_pad_push(base->srcpad, buffer);
    }
  }
  return GST_BASE_TRANSFORM_CLASS(recovery_transform_parent_class)->sink_event(base, event);
}

void recovery_transform_class_init(RecoveryTransformClass* klass) {
  GstCaps* caps = gst_caps_new_any();
  auto* element_class = GST_ELEMENT_CLASS(klass);
  gst_element_class_add_pad_template(element_class, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_element_class_add_pad_template(element_class, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
  auto* transform_class = GST_BASE_TRANSFORM_CLASS(klass);
  transform_class->submit_input_buffer = submit;
  transform_class->generate_output = generate;
  transform_class->sink_event = event;
}

void recovery_transform_init(RecoveryTransform* self) {
  self->cached = GST_FLOW_FLUSHING;
}

struct Fixture {
  RecoveryTransform* transform =
      reinterpret_cast<RecoveryTransform*>(g_object_new(recovery_transform_get_type(), nullptr));
  GstPad* source = gst_pad_new("source", GST_PAD_SRC);
  GstPad* sink = gst_pad_new("sink", GST_PAD_SINK);
  GstFlowReturn downstream = GST_FLOW_OK;
  guint received = 0;

  Fixture() {
    g_object_set_data(G_OBJECT(sink), "fixture", this);
    gst_pad_set_chain_function(
        sink, +[](GstPad* pad, GstObject*, GstBuffer* buffer) {
          auto* fixture = static_cast<Fixture*>(g_object_get_data(G_OBJECT(pad), "fixture"));
          ++fixture->received;
          gst_buffer_unref(buffer);
          return fixture->downstream;
        });
    gst_pad_set_event_function(
        sink, +[](GstPad*, GstObject*, GstEvent* event) -> gboolean {
          gst_event_unref(event);
          return TRUE;
        });
    gst_pad_set_active(source, TRUE);
    gst_pad_set_active(sink, TRUE);
    gst_element_set_state(GST_ELEMENT(transform), GST_STATE_PLAYING);
    gst_pad_link(source, transform->parent.sinkpad);
    gst_pad_link(transform->parent.srcpad, sink);
    gst_pad_push_event(source, gst_event_new_stream_start("test"));
    GstCaps* caps = gst_caps_new_empty_simple("application/x-hstream-test");
    gst_pad_push_event(source, gst_event_new_caps(caps));
    gst_caps_unref(caps);
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    gst_pad_push_event(source, gst_event_new_segment(&segment));
  }

  ~Fixture() {
    gst_element_set_state(GST_ELEMENT(transform), GST_STATE_NULL);
    if (transform->pending)
      gst_buffer_unref(transform->pending);
    gst_pad_set_active(source, FALSE);
    gst_pad_set_active(sink, FALSE);
    gst_object_unref(source);
    gst_object_unref(sink);
    gst_object_unref(transform);
  }

  bool arm() {
    return hm::ArmAsyncInferenceSeekRecovery(GST_ELEMENT(transform));
  }
  GstFlowReturn push() {
    return gst_pad_push(source, gst_buffer_new());
  }
};

bool expect(bool condition, const char* message) {
  if (!condition)
    std::fprintf(stderr, "%s\n", message);
  return condition;
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  bool ok = true;
  {
    Fixture fixture;
    ok &= expect(fixture.push() == GST_FLOW_FLUSHING, "Unprimed input must reproduce the stale flushing result");
    ok &= expect(fixture.transform->pending && fixture.received == 0, "Input was queued despite the flushing result");
  }
  {
    Fixture fixture;
    ok &= expect(fixture.arm() && fixture.arm(), "A newer seek must replace an unused recovery probe");
    ok &= expect(fixture.push() == GST_FLOW_OK, "First input must recover the current worker result");
    ok &= expect(
        fixture.received == 1 && fixture.transform->submissions == 1 && fixture.transform->barriers == 1,
        "Recovery must deliver the first buffer exactly once and drain exactly once");
    fixture.transform->immediate = TRUE;
    ok &= expect(
        fixture.push() == GST_FLOW_OK && fixture.received == 2 && fixture.transform->barriers == 1,
        "Later buffers must use the normal chain without a recovery barrier");
  }
  {
    Fixture fixture;
    fixture.downstream = GST_FLOW_ERROR;
    ok &= expect(fixture.arm() && fixture.push() == GST_FLOW_ERROR, "Recovery must propagate a real downstream error");
  }
  {
    Fixture fixture;
    fixture.transform->flush_during_submit = TRUE;
    ok &= expect(
        fixture.arm() && fixture.push() == GST_FLOW_FLUSHING && fixture.transform->barriers == 0,
        "A concurrent real flush must remain flushing without a drain attempt");
  }
  {
    Fixture fixture;
    fixture.transform->immediate = TRUE;
    ok &= expect(
        fixture.arm() && fixture.push() == GST_FLOW_OK && fixture.transform->barriers == 0,
        "An already healthy first buffer needs no barrier");
  }
  return ok ? 0 : 1;
}
