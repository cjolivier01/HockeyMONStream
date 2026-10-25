#include "hstream/src/gst-plugins/gst-amd-program/ProgramElement.h"

#include <atomic>
#include <exception>
#include <mutex>

namespace {
struct State {
  std::unique_ptr<hm::program::gst::Component> processor;
  GstCaps* output_caps{};
  std::atomic<guint64> frames{0};
  bool failed{false};
  ~State() {
    if (output_caps)
      gst_caps_unref(output_caps);
  }
};
struct HmProgramElement {
  GstElement parent;
  GstPad* sink;
  GstPad* src;
  State* state;
};
struct HmProgramElementClass {
  GstElementClass parent;
};
G_DEFINE_ABSTRACT_TYPE(HmProgramElement, hm_program_element, GST_TYPE_ELEMENT)

void Error(HmProgramElement* self, const std::string& message) {
  self->state->failed = true;
  GST_ELEMENT_ERROR(self, STREAM, FAILED, ("%s", message.c_str()), ("Program component %s", GST_OBJECT_NAME(self)));
}
GstFlowReturn Chain(GstPad*, GstObject* parent, GstBuffer* input) {
  auto* self = reinterpret_cast<HmProgramElement*>(parent);
  std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> owned(input, gst_buffer_unref);
  if (self->state->failed || !self->state->processor)
    return GST_FLOW_ERROR;
  try {
    // A tee/copy can share a header. Metadata transforms preserve owned GPU
    // frame lifetimes when making the envelope writable; no pixels are mapped.
    owned.reset(gst_buffer_make_writable(owned.release()));
    if (!owned) {
      Error(self, "Cannot make Program metadata writable");
      return GST_FLOW_ERROR;
    }
    auto result = self->state->processor->Process(owned.get());
    if (!result.ok() && absl::IsCancelled(result.status()))
      return GST_FLOW_FLUSHING;
    if (!result.ok() || !*result) {
      Error(self, result.ok() ? "Program component produced no output" : result.status().ToString());
      return GST_FLOW_ERROR;
    }
    auto* output = *result;
    if (output == owned.get())
      (void)owned.release();
    else
      owned.reset();
    owned.reset(output);
    if (auto* caps = self->state->processor->OutputCaps()) {
      auto* current = gst_pad_get_current_caps(self->src);
      const bool changed = !current || !gst_caps_is_equal(current, caps);
      if (current)
        gst_caps_unref(current);
      const bool accepted = !changed || gst_pad_push_event(self->src, gst_event_new_caps(caps));
      gst_caps_unref(caps);
      if (!accepted)
        return GST_FLOW_NOT_NEGOTIATED;
    }
    ++self->state->frames;
    return gst_pad_push(self->src, owned.release());
  } catch (const std::exception& error) {
    Error(self, error.what());
  } catch (...) {
    Error(self, "Unknown Program component exception");
  }
  return GST_FLOW_ERROR;
}
bool Reset(HmProgramElement* self) {
  if (self->state->failed)
    return false;
  try {
    if (self->state->processor) {
      const auto status = self->state->processor->Reset();
      if (!status.ok()) {
        Error(self, status.ToString());
        return false;
      }
    }
    self->state->frames = 0;
    return true;
  } catch (const std::exception& error) {
    Error(self, error.what());
  } catch (...) {
    Error(self, "Unknown Program reset exception");
  }
  return false;
}
gboolean Event(GstPad*, GstObject* parent, GstEvent* event) {
  auto* self = reinterpret_cast<HmProgramElement*>(parent);
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_START && self->state->processor)
    self->state->processor->Interrupt();
  if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP && !Reset(self)) {
    gst_event_unref(event);
    return FALSE;
  }
  if (GST_EVENT_TYPE(event) == GST_EVENT_CAPS && self->state->output_caps) {
    gst_event_unref(event);
    event = gst_event_new_caps(self->state->output_caps);
  }
  return gst_pad_push_event(self->src, event);
}
GstStateChangeReturn ChangeState(GstElement* element, GstStateChange transition) {
  auto* self = reinterpret_cast<HmProgramElement*>(element);
  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED && (!self->state->processor || self->state->failed)) {
    GST_ELEMENT_ERROR(self, CORE, STATE_CHANGE, ("Program component requires valid resolved configuration"), (nullptr));
    return GST_STATE_CHANGE_FAILURE;
  }
  if (transition == GST_STATE_CHANGE_PAUSED_TO_READY && self->state->processor)
    self->state->processor->Interrupt();
  if (transition == GST_STATE_CHANGE_READY_TO_PAUSED && !Reset(self))
    return GST_STATE_CHANGE_FAILURE;
  const auto result = GST_ELEMENT_CLASS(hm_program_element_parent_class)->change_state(element, transition);
  if (result != GST_STATE_CHANGE_FAILURE && transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    GST_PAD_STREAM_LOCK(self->sink);
    const bool reset = Reset(self);
    GST_PAD_STREAM_UNLOCK(self->sink);
    // A failed component must still allow teardown. It cannot be restarted.
    (void)reset;
  }
  return result;
}
void GetProperty(GObject* object, guint id, GValue* value, GParamSpec* spec) {
  const auto* self = reinterpret_cast<HmProgramElement*>(object);
  if (id == 1)
    g_value_set_uint64(value, self->state->frames.load());
  else
    G_OBJECT_WARN_INVALID_PROPERTY_ID(object, id, spec);
}
void Finalize(GObject* object) {
  auto* self = reinterpret_cast<HmProgramElement*>(object);
  delete self->state;
  G_OBJECT_CLASS(hm_program_element_parent_class)->finalize(object);
}
void hm_program_element_class_init(HmProgramElementClass* klass) {
  auto* object = G_OBJECT_CLASS(klass);
  object->get_property = GetProperty;
  object->finalize = Finalize;
  g_object_class_install_property(
      object,
      1,
      g_param_spec_uint64(
          "processed-frames",
          "Processed frames",
          "Completed frames in this timeline",
          0,
          G_MAXUINT64,
          0,
          GParamFlags(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));
  auto* element = GST_ELEMENT_CLASS(klass);
  element->change_state = ChangeState;
  static GstStaticPadTemplate sink = GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
  static GstStaticPadTemplate src = GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS_ANY);
  gst_element_class_add_static_pad_template(element, &sink);
  gst_element_class_add_static_pad_template(element, &src);
}
void hm_program_element_init(HmProgramElement* self) {
  self->state = new State;
  auto* klass = GST_ELEMENT_GET_CLASS(self);
  self->sink = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "sink"), "sink");
  self->src = gst_pad_new_from_template(gst_element_class_get_pad_template(klass, "src"), "src");
  gst_pad_set_chain_function(self->sink, GST_DEBUG_FUNCPTR(Chain));
  gst_pad_set_event_function(self->sink, GST_DEBUG_FUNCPTR(Event));
  gst_element_add_pad(GST_ELEMENT(self), self->sink);
  gst_element_add_pad(GST_ELEMENT(self), self->src);
}
#define PROGRAM_ELEMENT(Type, name, description)                                                \
  struct Type {                                                                                 \
    HmProgramElement parent;                                                                    \
  };                                                                                            \
  struct Type##Class {                                                                          \
    HmProgramElementClass parent;                                                               \
  };                                                                                            \
  G_DEFINE_TYPE(Type, name, hm_program_element_get_type())                                      \
  void name##_class_init(Type##Class* klass) {                                                  \
    gst_element_class_set_static_metadata(                                                      \
        GST_ELEMENT_CLASS(klass), description, "Filter/Video", description, "HockeyMONStream"); \
  }                                                                                             \
  void name##_init(Type*) {}
PROGRAM_ELEMENT(HmAmdStitch, hm_amd_stitch, "AMD GPU panorama stitching")
PROGRAM_ELEMENT(HmAmdInfer, hm_amd_infer, "AMD MIGraphX object detection")
PROGRAM_ELEMENT(HmRinkFilter, hm_rink_filter, "Calibrated rink detection filtering")
PROGRAM_ELEMENT(HmObjectTrack, hm_object_track, "Native motion and overlap object association")
PROGRAM_ELEMENT(HmCameraTrack, hm_camera_track, "Program camera policy")
PROGRAM_ELEMENT(HmAmdCrop, hm_amd_crop, "AMD GPU Program crop and overlays")
PROGRAM_ELEMENT(HmRinkMap, hm_rink_map, "GPU rink minimap output overlay")
#undef PROGRAM_ELEMENT

gboolean Register(GstPlugin* plugin) {
  return gst_element_register(plugin, "hmamdstitch", GST_RANK_NONE, hm_amd_stitch_get_type()) &&
      gst_element_register(plugin, "hmamdinfer", GST_RANK_NONE, hm_amd_infer_get_type()) &&
      gst_element_register(plugin, "hmrinkfilter", GST_RANK_NONE, hm_rink_filter_get_type()) &&
      gst_element_register(plugin, "hmobjecttrack", GST_RANK_NONE, hm_object_track_get_type()) &&
      gst_element_register(plugin, "hmcameratrack", GST_RANK_NONE, hm_camera_track_get_type()) &&
      gst_element_register(plugin, "hmamdcrop", GST_RANK_NONE, hm_amd_crop_get_type());
}
gboolean RegisterRinkMap(GstPlugin* plugin) {
  return gst_element_register(plugin, "hmrinkmap", GST_RANK_NONE, hm_rink_map_get_type());
}
bool RegisterRinkMapPlugin() {
  static std::once_flag once;
  static bool registered = false;
  std::call_once(once, [] {
    registered = gst_plugin_register_static(
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        "hstreamrinkmap",
        "GPU rink minimap output overlay",
        RegisterRinkMap,
        "1.0",
        "MIT/X11",
        "HockeyMONStream",
        "HockeyMONStream",
        "https://github.com/cjolivier01/HockeyMONStream");
  });
  return registered;
}
} // namespace
namespace hm::program::gst {
absl::StatusOr<GstBuffer*> WritableHeader(GstBuffer* input) {
  std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> owned(input, gst_buffer_unref);
  if (!input)
    return absl::InvalidArgumentError("Missing output buffer");
  if (gst_buffer_is_writable(input))
    return owned.release();
  std::unique_ptr<GstBuffer, decltype(&gst_buffer_unref)> header(gst_buffer_new(), gst_buffer_unref);
  if (!header)
    return absl::ResourceExhaustedError("Cannot allocate output buffer header");
  for (guint i = 0; i < gst_buffer_n_memory(input); ++i)
    gst_buffer_append_memory(header.get(), gst_memory_ref(gst_buffer_peek_memory(input, i)));
  if (!gst_buffer_copy_into(header.get(), input, GST_BUFFER_COPY_METADATA, 0, -1) ||
      !gst_buffer_add_parent_buffer_meta(header.get(), input))
    return absl::ResourceExhaustedError("Cannot retain output buffer metadata and pool ownership");
  return header.release();
}
bool RegisterProgramPlugin() {
  static std::once_flag once;
  static bool registered = false;
  std::call_once(once, [] {
    registered = gst_plugin_register_static(
        GST_VERSION_MAJOR,
        GST_VERSION_MINOR,
        "hstreamamdprogram",
        "Native AMD Program components",
        Register,
        "1.0",
        "MIT/X11",
        "HockeyMONStream",
        "HockeyMONStream",
        "https://github.com/cjolivier01/HockeyMONStream");
  });
  return registered;
}
absl::StatusOr<GstElement*> CreateElement(
    const char* factory,
    const char* name,
    std::unique_ptr<Component> processor,
    GstCaps* output_caps) {
  if (!processor || !(std::string(factory) == "hmrinkmap" ? RegisterRinkMapPlugin() : RegisterProgramPlugin()))
    return absl::InvalidArgumentError("Cannot register or configure Program component");
  auto* element = gst_element_factory_make(factory, name);
  if (!element)
    return absl::UnavailableError("Missing Program component factory");
  if (!G_TYPE_CHECK_INSTANCE_TYPE(element, hm_program_element_get_type())) {
    gst_object_unref(element);
    return absl::InvalidArgumentError("Unexpected Program component factory type");
  }
  auto* self = reinterpret_cast<HmProgramElement*>(element);
  self->state->processor = std::move(processor);
  self->state->output_caps = output_caps ? gst_caps_ref(output_caps) : nullptr;
  return element;
}
} // namespace hm::program::gst
