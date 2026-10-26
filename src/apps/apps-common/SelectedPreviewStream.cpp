#include "SelectedPreviewStream.h"

#include <mutex>
#include <new>
#include <string>

namespace {
using hm::preview::FrameIdentityMeta;

GType identity_api() {
  static const GType type = [] {
    static const gchar* tags[] = {nullptr};
    return gst_meta_api_type_register("HStreamPreviewFrameIdentityAPI", tags);
  }();
  return type;
}

const GstMetaInfo* identity_info();

gboolean identity_init(GstMeta* base, gpointer, GstBuffer*) {
  auto* meta = reinterpret_cast<FrameIdentityMeta*>(base);
  meta->channel = nullptr;
  meta->operation = 0;
  meta->sequence = 0;
  meta->source_pts = GST_CLOCK_TIME_NONE;
  meta->presentation_pts = GST_CLOCK_TIME_NONE;
  return TRUE;
}

void identity_free(GstMeta* base, GstBuffer*) {
  auto* meta = reinterpret_cast<FrameIdentityMeta*>(base);
  if (meta->channel)
    g_ref_string_release(meta->channel);
}

gboolean identity_transform(GstBuffer* output, GstMeta* base, GstBuffer*, GQuark type, gpointer) {
  if (!GST_META_TRANSFORM_IS_COPY(type))
    return FALSE;
  const auto* original = reinterpret_cast<FrameIdentityMeta*>(base);
  auto* copy = reinterpret_cast<FrameIdentityMeta*>(gst_buffer_add_meta(output, identity_info(), nullptr));
  if (!copy)
    return FALSE;
  copy->channel = original->channel ? g_ref_string_acquire(original->channel) : nullptr;
  copy->operation = original->operation;
  copy->sequence = original->sequence;
  copy->source_pts = original->source_pts;
  copy->presentation_pts = original->presentation_pts;
  return TRUE;
}

const GstMetaInfo* identity_info() {
  static const GstMetaInfo* info = gst_meta_register(
      identity_api(),
      "HStreamPreviewFrameIdentity",
      sizeof(FrameIdentityMeta),
      identity_init,
      identity_free,
      identity_transform);
  return info;
}

struct RouteState {
  explicit RouteState(const char* name) : channel(g_ref_string_new(name)) {}
  ~RouteState() {
    g_ref_string_release(channel);
    if (caps)
      gst_caps_unref(caps);
  }
  gchar* channel;
  std::mutex mutex;
  GstCaps* caps{nullptr};
  bool have_segment{false};
  bool ended{false};
  bool invalid_caps{false};
};

struct StreamState {
  ~StreamState() {
    if (output_caps)
      gst_caps_unref(output_caps);
  }
  std::mutex flow;
  std::string stream_id;
  std::string memory_feature;
  std::string eof_channel;
  GstPad* selected{nullptr}; // Parent element owns route pads.
  std::uint64_t operation{0};
  std::uint64_t sequence{0};
  GstClockTime last_pts{GST_CLOCK_TIME_NONE};
  GstCaps* output_caps{nullptr};
  bool started{false};
  bool segmented{false};
  bool ended{false};
  bool failed{false};
};

typedef struct _GstHmSelectedPreview {
  GstElement parent;
  GstPad* src;
  StreamState* state;
} GstHmSelectedPreview;
typedef struct _GstHmSelectedPreviewClass {
  GstElementClass parent_class;
} GstHmSelectedPreviewClass;
G_DEFINE_TYPE(GstHmSelectedPreview, gst_hm_selected_preview, GST_TYPE_ELEMENT)

constexpr const char* kRouteData = "hstream-selected-preview-route";
GstStaticPadTemplate source_template =
    GST_STATIC_PAD_TEMPLATE("src", GST_PAD_SRC, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw(ANY)"));
GstStaticPadTemplate route_template =
    GST_STATIC_PAD_TEMPLATE("route_%u", GST_PAD_SINK, GST_PAD_SOMETIMES, GST_STATIC_CAPS("video/x-raw(ANY)"));

GstHmSelectedPreview* get_stream(GstElement* stream) {
  return stream && G_TYPE_CHECK_INSTANCE_TYPE(stream, gst_hm_selected_preview_get_type())
      ? reinterpret_cast<GstHmSelectedPreview*>(stream)
      : nullptr;
}
RouteState* get_route(GstPad* pad) {
  return pad ? static_cast<RouteState*>(g_object_get_data(G_OBJECT(pad), kRouteData)) : nullptr;
}

bool allowed_caps(StreamState* state, GstCaps* caps) {
  if (!caps || !gst_caps_is_fixed(caps) || gst_caps_get_size(caps) != 1)
    return false;
  return gst_structure_has_name(gst_caps_get_structure(caps, 0), "video/x-raw") &&
      !gst_caps_features_is_any(gst_caps_get_features(caps, 0)) &&
      gst_caps_features_contains(gst_caps_get_features(caps, 0), state->memory_feature.c_str());
}

GstClockTime running_time(GstElement* element) {
  GstClock* clock = gst_element_get_clock(element);
  if (!clock)
    return GST_CLOCK_TIME_NONE;
  const auto now = gst_clock_get_time(clock);
  const auto base = gst_element_get_base_time(element);
  gst_object_unref(clock);
  return GST_CLOCK_TIME_IS_VALID(now) && GST_CLOCK_TIME_IS_VALID(base) && now >= base ? now - base
                                                                                      : GST_CLOCK_TIME_NONE;
}

bool send_headers(GstHmSelectedPreview* self, GstCaps* caps) {
  auto* state = self->state;
  if (!state->started) {
    if (!gst_pad_push_event(self->src, gst_event_new_stream_start(state->stream_id.c_str())))
      return false;
    state->started = true;
  }
  if (!state->output_caps || !gst_caps_is_equal(state->output_caps, caps)) {
    if (!gst_pad_push_event(self->src, gst_event_new_caps(caps)))
      return false;
    gst_caps_replace(&state->output_caps, caps);
  }
  if (!state->segmented) {
    GstSegment segment;
    gst_segment_init(&segment, GST_FORMAT_TIME);
    if (!gst_pad_push_event(self->src, gst_event_new_segment(&segment)))
      return false;
    state->segmented = true;
  }
  return true;
}

GstFlowReturn route_chain(GstPad* pad, GstObject* parent, GstBuffer* original) noexcept {
  GstBuffer* output = nullptr;
  try {
    auto* self = reinterpret_cast<GstHmSelectedPreview*>(parent);
    auto* state = self ? self->state : nullptr;
    auto* route = get_route(pad);
    // Query the pipeline clock outside the flow lock: state transitions can
    // hold the element object lock while waiting for a stream callback.
    const GstClockTime clock_pts = running_time(GST_ELEMENT(self));
    if (!state || !route) {
      gst_buffer_unref(original);
      return GST_FLOW_ERROR;
    }
    std::lock_guard<std::mutex> lock(state->flow);
    if (state->selected != pad || state->ended || !GST_CLOCK_TIME_IS_VALID(clock_pts)) {
      gst_buffer_unref(original);
      return GST_FLOW_OK;
    }
    GstCaps* caps = nullptr;
    {
      std::lock_guard<std::mutex> route_lock(route->mutex);
      if (state->failed || route->invalid_caps) {
        gst_buffer_unref(original);
        return GST_FLOW_NOT_NEGOTIATED;
      }
      if (route->ended || !route->caps || !route->have_segment) {
        gst_buffer_unref(original);
        return GST_FLOW_OK;
      }
      caps = gst_caps_ref(route->caps);
    }
    const bool headers_sent = send_headers(self, caps);
    gst_caps_unref(caps);
    if (!headers_sent) {
      state->failed = true;
      gst_buffer_unref(original);
      return GST_FLOW_NOT_NEGOTIATED;
    }
    GstClockTime pts = clock_pts;
    if (GST_CLOCK_TIME_IS_VALID(state->last_pts) && pts <= state->last_pts)
      pts = state->last_pts + 1;
    // Explicitly reference each memory block. gst_buffer_copy/make_writable may
    // copy NO_SHARE memory; this path must never copy raw GPU video to the CPU.
    output = gst_buffer_new();
    if (!output ||
        !gst_buffer_copy_into(
            output,
            original,
            static_cast<GstBufferCopyFlags>(GST_BUFFER_COPY_FLAGS | GST_BUFFER_COPY_TIMESTAMPS),
            0,
            -1)) {
      if (output)
        gst_buffer_unref(output);
      gst_buffer_unref(original);
      return GST_FLOW_ERROR;
    }
    for (guint i = 0; i < gst_buffer_n_memory(original); ++i)
      gst_buffer_append_memory(output, gst_memory_ref(gst_buffer_peek_memory(original, i)));
    if (!gst_buffer_add_parent_buffer_meta(output, original)) {
      gst_buffer_unref(output);
      gst_buffer_unref(original);
      return GST_FLOW_ERROR;
    }
    // COPY_META without COPY_MEMORY intentionally drops memory-layout metadata.
    // We retained the exact complete memory layout ourselves, so invoke each
    // copy transform after attaching those references (also needed by GL sync
    // and native batch metadata). Opaque metadata remains on the retained source
    // for backend-specific consumers; it must not reject unrelated valid video.
    gpointer iterator = nullptr;
    while (GstMeta* meta = gst_buffer_iterate_meta(original, &iterator)) {
      if (meta->info->api == identity_api() || meta->info->api == GST_PARENT_BUFFER_META_API_TYPE)
        continue;
      GstMetaTransformCopy copy{FALSE, 0, gst_buffer_get_size(original)};
      if (meta->info->transform_func)
        meta->info->transform_func(output, meta, original, _gst_meta_transform_copy, &copy);
    }
    auto* identity = reinterpret_cast<FrameIdentityMeta*>(gst_buffer_add_meta(output, identity_info(), nullptr));
    if (!identity) {
      gst_buffer_unref(output);
      gst_buffer_unref(original);
      return GST_FLOW_ERROR;
    }
    identity->channel = g_ref_string_acquire(route->channel);
    identity->operation = state->operation;
    identity->sequence = ++state->sequence;
    identity->source_pts = GST_BUFFER_PTS(original);
    identity->presentation_pts = pts;
    GST_BUFFER_PTS(output) = pts;
    GST_BUFFER_DTS(output) = GST_CLOCK_TIME_NONE;
    state->last_pts = pts;
    gst_buffer_unref(original);
    original = nullptr;
    GstBuffer* admitted = output;
    output = nullptr;
    const GstFlowReturn result = gst_pad_push(self->src, admitted);
    if (result != GST_FLOW_OK && result != GST_FLOW_FLUSHING)
      state->failed = true;
    return result;
  } catch (...) {
    if (original)
      gst_buffer_unref(original);
    if (output)
      gst_buffer_unref(output);
    return GST_FLOW_ERROR;
  }
}

gboolean route_event(GstPad* pad, GstObject* parent, GstEvent* event) noexcept {
  try {
    auto* self = reinterpret_cast<GstHmSelectedPreview*>(parent);
    auto* route = get_route(pad);
    if (self && self->state && route) {
      bool output_eof = false;
      {
        std::lock_guard<std::mutex> lock(route->mutex);
        switch (GST_EVENT_TYPE(event)) {
          case GST_EVENT_STREAM_START:
            route->ended = false;
            route->have_segment = false;
            break;
          case GST_EVENT_CAPS: {
            GstCaps* caps = nullptr;
            gst_event_parse_caps(event, &caps);
            route->invalid_caps = !allowed_caps(self->state, caps);
            gst_caps_replace(&route->caps, route->invalid_caps ? nullptr : caps);
            break;
          }
          case GST_EVENT_SEGMENT: {
            const GstSegment* segment = nullptr;
            gst_event_parse_segment(event, &segment);
            route->have_segment = segment && segment->format == GST_FORMAT_TIME;
            break;
          }
          case GST_EVENT_FLUSH_START:
          case GST_EVENT_FLUSH_STOP:
            route->have_segment = false;
            route->ended = false;
            break;
          case GST_EVENT_EOS:
            route->ended = true;
            output_eof = self->state->eof_channel == route->channel;
            break;
          default:
            break;
        }
      }
      // Inactive route events never wait behind a selected GPU frame. Only the
      // authoritative output EOF drains the consumer's admitted work.
      if (output_eof) {
        std::lock_guard<std::mutex> lock(self->state->flow);
        if (!self->state->ended) {
          self->state->ended = true;
          gst_pad_push_event(self->src, gst_event_new_eos());
        }
      }
    }
  } catch (...) {
    gst_event_unref(event);
    return FALSE;
  }
  gst_event_unref(event);
  return TRUE;
}

gboolean route_query(GstPad* pad, GstObject* parent, GstQuery* query) noexcept {
  auto* self = reinterpret_cast<GstHmSelectedPreview*>(parent);
  if (!self || !self->state)
    return FALSE;
  if (GST_QUERY_TYPE(query) == GST_QUERY_CAPS) {
    GstCaps* filter = nullptr;
    gst_query_parse_caps(query, &filter);
    GstCaps* caps = gst_caps_new_empty_simple("video/x-raw");
    gst_caps_set_features(caps, 0, gst_caps_features_new(self->state->memory_feature.c_str(), nullptr));
    GstCaps* result = filter ? gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST) : gst_caps_ref(caps);
    gst_query_set_caps_result(query, result);
    gst_caps_unref(result);
    gst_caps_unref(caps);
    return TRUE;
  }
  if (GST_QUERY_TYPE(query) == GST_QUERY_ACCEPT_CAPS) {
    GstCaps* caps = nullptr;
    gst_query_parse_accept_caps(query, &caps);
    gst_query_set_accept_caps_result(query, allowed_caps(self->state, caps));
    return TRUE;
  }
  return GST_PAD_DIRECTION(pad) == GST_PAD_SINK ? gst_pad_peer_query(self->src, query)
                                                : gst_pad_query_default(pad, parent, query);
}

gboolean output_event(GstPad*, GstObject*, GstEvent* event) noexcept {
  // This observational consumer must not seek or send QoS upstream into the
  // processing/archive graph. Native owners explicitly control its routes.
  const bool accepted = GST_EVENT_TYPE(event) != GST_EVENT_SEEK;
  gst_event_unref(event);
  return accepted;
}

void finalize(GObject* object) noexcept {
  auto* self = reinterpret_cast<GstHmSelectedPreview*>(object);
  delete self->state;
  self->state = nullptr;
  G_OBJECT_CLASS(gst_hm_selected_preview_parent_class)->finalize(object);
}

void gst_hm_selected_preview_class_init(GstHmSelectedPreviewClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = finalize;
  auto* element = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(
      element,
      "HStream selected preview stream",
      "Generic",
      "Isolates native preview routes and owns one presentation timeline",
      "HStream");
  gst_element_class_add_static_pad_template(element, &source_template);
  gst_element_class_add_static_pad_template(element, &route_template);
}

void gst_hm_selected_preview_init(GstHmSelectedPreview* self) {
  self->state = new (std::nothrow) StreamState;
  self->src = gst_pad_new_from_static_template(&source_template, "src");
  gst_pad_set_query_function(self->src, route_query);
  gst_pad_set_event_function(self->src, output_event);
  gst_element_add_pad(GST_ELEMENT(self), self->src);
}
} // namespace

namespace hm::preview {
const FrameIdentityMeta* frame_identity(GstBuffer* buffer) {
  return buffer ? reinterpret_cast<const FrameIdentityMeta*>(gst_buffer_get_meta(buffer, identity_api())) : nullptr;
}

bool copy_frame_identity(GstBuffer* output, GstBuffer* input) {
  const auto* identity = frame_identity(input);
  if (!output || !identity || frame_identity(output))
    return false;
  GstMetaTransformCopy copy{FALSE, 0, gst_buffer_get_size(input)};
  return identity_transform(output, const_cast<GstMeta*>(&identity->meta), input, _gst_meta_transform_copy, &copy);
}

GstBuffer* source_frame(GstBuffer* buffer) {
  if (!frame_identity(buffer))
    return nullptr;
  auto* parent = gst_buffer_get_parent_buffer_meta(buffer);
  return parent ? parent->buffer : nullptr;
}

GstElement* selected_stream_create(
    const char* name,
    const char* stream_id,
    const char* memory_feature,
    const char* eof_channel) {
  if (!stream_id || !*stream_id || !memory_feature || !*memory_feature || !eof_channel || !*eof_channel)
    return nullptr;
  auto* self =
      reinterpret_cast<GstHmSelectedPreview*>(g_object_new(gst_hm_selected_preview_get_type(), "name", name, nullptr));
  if (!self || !self->state) {
    if (self)
      gst_object_unref(self);
    return nullptr;
  }
  self->state->stream_id = stream_id;
  self->state->memory_feature = memory_feature;
  self->state->eof_channel = eof_channel;
  return GST_ELEMENT(self);
}

GstPad* selected_stream_add_route(GstElement* stream, const char* channel) {
  auto* self = get_stream(stream);
  if (!self || !self->state || !channel || !*channel)
    return nullptr;
  if (GstPad* existing = gst_element_get_static_pad(stream, channel)) {
    gst_object_unref(existing);
    return nullptr;
  }
  GstPad* pad = gst_pad_new_from_static_template(&route_template, channel);
  g_object_set_data_full(
      G_OBJECT(pad), kRouteData, new RouteState(channel), +[](gpointer data) {
        delete static_cast<RouteState*>(data);
      });
  gst_pad_set_chain_function(pad, route_chain);
  gst_pad_set_event_function(pad, route_event);
  gst_pad_set_query_function(pad, route_query);
  if (!gst_element_add_pad(stream, pad)) {
    gst_object_unref(pad);
    return nullptr;
  }
  if (GST_STATE(stream) >= GST_STATE_PAUSED)
    gst_pad_set_active(pad, TRUE);
  return GST_PAD(gst_object_ref(pad));
}

void selected_stream_remove_route(GstElement* stream, GstPad* pad) {
  auto* self = get_stream(stream);
  if (!self || !self->state || !pad || GST_OBJECT_PARENT(pad) != GST_OBJECT(stream))
    return;
  {
    std::lock_guard<std::mutex> lock(self->state->flow);
    if (self->state->selected == pad)
      self->state->selected = nullptr;
  }
  gst_pad_set_active(pad, FALSE);
  gst_element_remove_pad(stream, pad);
}

bool selected_stream_select(GstElement* stream, GstPad* pad, std::uint64_t operation) {
  auto* self = get_stream(stream);
  if (!self || !self->state || operation == 0 ||
      (pad && (GST_OBJECT_PARENT(pad) != GST_OBJECT(stream) || !get_route(pad))))
    return false;
  std::lock_guard<std::mutex> lock(self->state->flow);
  auto* state = self->state;
  if (state->ended || state->failed || operation < state->operation ||
      (operation == state->operation && state->selected && pad && state->selected != pad))
    return false;
  if (pad) {
    auto* route = get_route(pad);
    std::lock_guard<std::mutex> route_lock(route->mutex);
    if (route->ended || route->invalid_caps)
      return false;
  }
  state->selected = pad;
  state->operation = operation;
  return true;
}

bool selected_stream_active(GstElement* stream, GstPad* pad, std::uint64_t operation) {
  auto* self = get_stream(stream);
  if (!self || !self->state || !pad)
    return false;
  std::lock_guard<std::mutex> lock(self->state->flow);
  auto* route = get_route(pad);
  if (!route)
    return false;
  std::lock_guard<std::mutex> route_lock(route->mutex);
  return self->state->selected == pad && self->state->operation == operation && !self->state->ended &&
      !self->state->failed && !route->ended && !route->invalid_caps;
}
} // namespace hm::preview
