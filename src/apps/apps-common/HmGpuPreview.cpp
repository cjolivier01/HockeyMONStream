#include "HmGpuPreview.h"

#include "hstream/src/apps/apps-common/RinkMaskImage.h"
#include "hstream/src/libs/common/PreviewOverlayMeta.h"

#include <gst/base/gstbasesink.h>
#include <gst/video/video.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <mutex>
#include <string>

namespace {

constexpr const char* kStatusStructureName = "hstream-preview-status";

void post_preview_status(
    GstElement* element,
    const char* channel,
    const char* status,
    std::uint64_t generation,
    const char* message) {
  const char* safe_channel = channel && *channel ? channel : "unknown";
  const char* safe_message = message ? message : "";
  GstStructure* structure = gst_structure_new(
      kStatusStructureName,
      "channel",
      G_TYPE_STRING,
      safe_channel,
      "status",
      G_TYPE_STRING,
      status,
      "generation",
      G_TYPE_UINT64,
      static_cast<guint64>(generation),
      "message",
      G_TYPE_STRING,
      safe_message,
      nullptr);
  gst_element_post_message(element, gst_message_new_application(GST_OBJECT(element), structure));
  g_print(
      "HSTREAM_PREVIEW channel=%s status=%s generation=%" G_GUINT64_FORMAT " message=%s\n",
      safe_channel,
      status,
      static_cast<guint64>(generation),
      safe_message);
}

struct IsolationState {
  IsolationState() {
    g_weak_ref_init(&failure_peer, nullptr);
  }

  ~IsolationState() {
    g_weak_ref_clear(&failure_peer);
  }

  std::atomic<bool> active{false};
  std::atomic<bool> failed{false};
  std::atomic<std::uint64_t> generation{0};
  // Serializes active-state changes with buffers crossing the gate. When an
  // active=false property update returns, no buffer can still be downstream
  // of this element on its way to the converter or renderer.
  std::mutex flow_mutex;
  std::mutex channel_mutex;
  std::string channel{"unknown"};
  GWeakRef failure_peer;
};

typedef struct _GstHmPreviewIsolation {
  GstElement parent;
  GstPad* sink_pad;
  GstPad* src_pad;
  IsolationState* state;
} GstHmPreviewIsolation;

typedef struct _GstHmPreviewIsolationClass {
  GstElementClass parent_class;
} GstHmPreviewIsolationClass;

G_DEFINE_TYPE(GstHmPreviewIsolation, gst_hm_preview_isolation, GST_TYPE_ELEMENT)

enum IsolationProperty {
  kIsolationPropertyNone,
  kIsolationPropertyActive,
  kIsolationPropertyChannel,
  kIsolationPropertyGeneration,
};

const char* isolation_channel(IsolationState* state, std::string* storage) {
  std::lock_guard<std::mutex> lock(state->channel_mutex);
  *storage = state->channel;
  return storage->c_str();
}

void fail_isolation(GstHmPreviewIsolation* self, GstFlowReturn flow) {
  IsolationState* state = self->state;
  bool expected = false;
  if (!state->failed.compare_exchange_strong(expected, true))
    return;
  state->active = false;
  auto* failure_peer = static_cast<GObject*>(g_weak_ref_get(&state->failure_peer));
  if (failure_peer) {
    g_object_set(failure_peer, "active", FALSE, nullptr);
    g_object_unref(failure_peer);
  }
  std::string channel;
  const std::string message = "downstream preview flow failed: " + std::string(gst_flow_get_name(flow));
  post_preview_status(
      GST_ELEMENT(self), isolation_channel(state, &channel), "failed", state->generation.load(), message.c_str());
}

GstFlowReturn isolation_chain(GstPad*, GstObject* parent, GstBuffer* buffer) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(parent);
  if (self->state->failed.load(std::memory_order_relaxed) || !self->state->active.load(std::memory_order_acquire)) {
    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
  }
  std::lock_guard<std::mutex> lock(self->state->flow_mutex);
  if (self->state->failed.load() || !self->state->active.load()) {
    gst_buffer_unref(buffer);
    return GST_FLOW_OK;
  }
  const GstFlowReturn flow = gst_pad_push(self->src_pad, buffer);
  // FLUSHING is expected while the application performs its startup seek or
  // changes state. It is not a preview failure and must not permanently close
  // the observational branch.
  if (flow == GST_FLOW_OK || flow == GST_FLOW_FLUSHING)
    return GST_FLOW_OK;
  fail_isolation(self, flow);
  return GST_FLOW_OK;
}

GstFlowReturn isolation_chain_list(GstPad*, GstObject* parent, GstBufferList* buffers) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(parent);
  if (self->state->failed.load(std::memory_order_relaxed) || !self->state->active.load(std::memory_order_acquire)) {
    gst_buffer_list_unref(buffers);
    return GST_FLOW_OK;
  }
  std::lock_guard<std::mutex> lock(self->state->flow_mutex);
  if (self->state->failed.load() || !self->state->active.load()) {
    gst_buffer_list_unref(buffers);
    return GST_FLOW_OK;
  }
  const GstFlowReturn flow = gst_pad_push_list(self->src_pad, buffers);
  if (flow == GST_FLOW_OK || flow == GST_FLOW_FLUSHING)
    return GST_FLOW_OK;
  fail_isolation(self, flow);
  return GST_FLOW_OK;
}

gboolean isolation_event(GstPad*, GstObject* parent, GstEvent* event) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(parent);
  gst_pad_push_event(self->src_pad, event);
  // Preview descendants are observational. Event rejection must never
  // prevent global negotiation, FLUSH, or EOS completion upstream.
  return TRUE;
}

gboolean isolation_query(GstPad*, GstObject* parent, GstQuery* query) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(parent);
  return gst_pad_peer_query(self->src_pad, query);
}

void isolation_set_property(GObject* object, guint property_id, const GValue* value, GParamSpec* spec) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(object);
  switch (property_id) {
    case kIsolationPropertyActive: {
      if (!g_value_get_boolean(value)) {
        auto* test_probe =
            static_cast<std::atomic<bool>*>(g_object_get_data(object, "hstream-preview-test-active-setter-entered"));
        if (test_probe)
          test_probe->store(true, std::memory_order_release);
      }
      std::lock_guard<std::mutex> lock(self->state->flow_mutex);
      if (!self->state->failed.load())
        self->state->active = g_value_get_boolean(value);
      break;
    }
    case kIsolationPropertyChannel: {
      std::lock_guard<std::mutex> lock(self->state->channel_mutex);
      self->state->channel = g_value_get_string(value) ? g_value_get_string(value) : "unknown";
      break;
    }
    case kIsolationPropertyGeneration:
      self->state->generation = g_value_get_uint64(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
  }
}

void isolation_get_property(GObject* object, guint property_id, GValue* value, GParamSpec* spec) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(object);
  switch (property_id) {
    case kIsolationPropertyActive:
      g_value_set_boolean(value, self->state->active.load());
      break;
    case kIsolationPropertyChannel: {
      std::lock_guard<std::mutex> lock(self->state->channel_mutex);
      g_value_set_string(value, self->state->channel.c_str());
      break;
    }
    case kIsolationPropertyGeneration:
      g_value_set_uint64(value, self->state->generation.load());
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
  }
}

void isolation_finalize(GObject* object) {
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(object);
  delete self->state;
  self->state = nullptr;
  G_OBJECT_CLASS(gst_hm_preview_isolation_parent_class)->finalize(object);
}

void gst_hm_preview_isolation_class_init(GstHmPreviewIsolationClass* klass) {
  auto* object_class = G_OBJECT_CLASS(klass);
  auto* element_class = GST_ELEMENT_CLASS(klass);
  object_class->set_property = isolation_set_property;
  object_class->get_property = isolation_get_property;
  object_class->finalize = isolation_finalize;
  g_object_class_install_property(
      object_class,
      kIsolationPropertyActive,
      g_param_spec_boolean("active", "Active", "Forward preview buffers", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kIsolationPropertyChannel,
      g_param_spec_string("channel", "Channel", "Logical preview channel", "unknown", G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kIsolationPropertyGeneration,
      g_param_spec_uint64(
          "generation", "Generation", "Preview activation generation", 0, G_MAXUINT64, 0, G_PARAM_READWRITE));
  GstCaps* caps = gst_caps_new_any();
  gst_element_class_add_pad_template(element_class, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_element_class_add_pad_template(element_class, gst_pad_template_new("src", GST_PAD_SRC, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
  gst_element_class_set_static_metadata(
      element_class,
      "HStream preview flow isolation",
      "Generic",
      "Drops inactive preview buffers and contains downstream preview flow failures",
      "HStream");
}

void gst_hm_preview_isolation_init(GstHmPreviewIsolation* self) {
  self->state = new IsolationState();
  GstPadTemplate* sink_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(self), "sink");
  GstPadTemplate* src_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(self), "src");
  self->sink_pad = gst_pad_new_from_template(sink_template, "sink");
  self->src_pad = gst_pad_new_from_template(src_template, "src");
  gst_pad_set_chain_function(self->sink_pad, GST_DEBUG_FUNCPTR(isolation_chain));
  gst_pad_set_chain_list_function(self->sink_pad, GST_DEBUG_FUNCPTR(isolation_chain_list));
  gst_pad_set_event_function(self->sink_pad, GST_DEBUG_FUNCPTR(isolation_event));
  gst_pad_set_query_function(self->sink_pad, GST_DEBUG_FUNCPTR(isolation_query));
  gst_element_add_pad(GST_ELEMENT(self), self->sink_pad);
  gst_element_add_pad(GST_ELEMENT(self), self->src_pad);
}

} // namespace

#if defined(__x86_64__)

#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glx.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <cuda_gl_interop.h>
#include <cuda_runtime_api.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <nvdsmeta.h>
#include <array>
#include <chrono>
#include <optional>
#include <thread>
#include <vector>

namespace {

constexpr auto kGpuCompletionTimeout = std::chrono::seconds(5);

struct OverlayColor {
  float red{1.0F};
  float green{1.0F};
  float blue{1.0F};
  float alpha{1.0F};
};

struct OverlayPath {
  std::vector<hm::preview_overlay::Point> points;
  OverlayColor color;
  float width{1.0F};
  bool closed{false};
  bool filled{false};
};

struct PreviewOverlays {
  std::vector<OverlayPath> paths;
  std::optional<hm::preview_overlay::PlayCropperTransform> program_transform;
  float coordinate_width{0.0F};
  float coordinate_height{0.0F};
};

struct RendererState {
  std::mutex mutex;
  std::atomic<bool> stopping{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> failure_reported{false};
  std::atomic<std::uint64_t> x_error_serial{0};
  std::string channel{"unknown"};
  guint64 window_id{0};
  guint gpu_id{0};
  guint source_width{0};
  guint source_height{0};
  guint negotiated_width{0};
  guint negotiated_height{0};
  std::atomic<guint64> generation{0};
  guint64 ready_generation{G_MAXUINT64};
  GstVideoInfo video_info{};
  bool have_caps{false};
  Display* display{nullptr};
  GLXContext context{nullptr};
  GLuint texture{0};
  cudaGraphicsResource_t cuda_texture{nullptr};
  cudaStream_t cuda_stream{nullptr};
  cudaEvent_t copy_complete{nullptr};
  std::atomic<bool> show_player_tracking{false};
  std::atomic<bool> show_play_tracking{false};
  std::atomic<bool> show_rink_mask{false};
  std::string rink_mask_file;
  bool rink_mask_dirty{true};
  GLuint rink_mask_texture{0};
  guint rink_mask_width{0};
  guint rink_mask_height{0};
  bool rink_mask_failure_reported{false};
  unsigned rink_mask_consecutive_failures{0};
  std::chrono::steady_clock::time_point rink_mask_retry_after{};
};

typedef struct _GstHmGpuPreviewSink {
  GstBaseSink parent;
  RendererState* state;
} GstHmGpuPreviewSink;

typedef struct _GstHmGpuPreviewSinkClass {
  GstBaseSinkClass parent_class;
} GstHmGpuPreviewSinkClass;

G_DEFINE_TYPE(GstHmGpuPreviewSink, gst_hm_gpu_preview_sink, GST_TYPE_BASE_SINK)

enum SinkProperty {
  kSinkPropertyNone,
  kSinkPropertyWindowId,
  kSinkPropertyGpuId,
  kSinkPropertyChannel,
  kSinkPropertyGeneration,
  kSinkPropertySourceWidth,
  kSinkPropertySourceHeight,
  kSinkPropertyShowPlayerTracking,
  kSinkPropertyShowPlayTracking,
  kSinkPropertyShowRinkMask,
  kSinkPropertyRinkMaskFile,
};

thread_local RendererState* current_x_error_target = nullptr;

int preview_x_error_handler(Display* display, XErrorEvent* event) {
  if (current_x_error_target) {
    current_x_error_target->failed = true;
    ++current_x_error_target->x_error_serial;
    return 0;
  }
  char message[256]{};
  XGetErrorText(display, event->error_code, message, sizeof(message));
  g_printerr(
      "HSTREAM_PREVIEW_X11_ERROR code=%u request=%u minor=%u resource=%lu message=%s\n",
      event->error_code,
      event->request_code,
      event->minor_code,
      event->resourceid,
      message);
  // X11 drawables are owned by the UI and can disappear asynchronously.
  // Never fall through to Xlib's default process-terminating handler.
  return 0;
}

void initialize_xlib_once() {
  static std::once_flag once;
  std::call_once(once, [] {
    XInitThreads();
    XSetErrorHandler(preview_x_error_handler);
  });
}

void post_sink_failure(GstHmGpuPreviewSink* self, const char* message) {
  RendererState* state = self->state;
  state->failed = true;
  bool expected = false;
  if (state->failure_reported.compare_exchange_strong(expected, true)) {
    post_preview_status(
        GST_ELEMENT(self),
        state->channel.c_str(),
        "failed",
        state->generation.load(),
        message ? message : "renderer failed");
  }
}

bool cuda_succeeded(GstHmGpuPreviewSink* self, cudaError_t result, const char* operation) {
  if (result == cudaSuccess)
    return true;
  const std::string message = std::string(operation) + ": " + cudaGetErrorString(result);
  post_sink_failure(self, message.c_str());
  return false;
}

[[noreturn]] void terminate_for_unsafe_cuda_state(
    GstHmGpuPreviewSink* self,
    const char* operation,
    cudaError_t result) {
  RendererState* state = self->state;
  g_printerr(
      "HSTREAM_PREVIEW_FATAL channel=%s operation=%s message=%s\n",
      state->channel.c_str(),
      operation,
      cudaGetErrorString(result));
  // Once an interop resource is mapped, CUDA may retain access to the GL
  // texture or the borrowed NvBufSurface even when a later API call fails.
  // Exiting without unwinding is the only path that cannot return a still-used
  // surface to the GStreamer pool.
  std::_Exit(86);
}

bool make_context_current(GstHmGpuPreviewSink* self) {
  RendererState* state = self->state;
  if (!state->display || !state->context || state->window_id == 0)
    return false;
  const std::uint64_t error_serial = state->x_error_serial.load();
  current_x_error_target = state;
  const Bool current = glXMakeCurrent(state->display, static_cast<GLXDrawable>(state->window_id), state->context);
  XSync(state->display, False);
  current_x_error_target = nullptr;
  if (!current || state->x_error_serial.load() != error_serial) {
    if (current)
      glXMakeCurrent(state->display, None, nullptr);
    post_sink_failure(self, "could not make GLX context current on the preview window");
    return false;
  }
  return true;
}

void release_context(RendererState* state) {
  if (state->display)
    glXMakeCurrent(state->display, None, nullptr);
}

bool initialize_renderer(GstHmGpuPreviewSink* self) {
  RendererState* state = self->state;
  if (state->context)
    return true;
  if (!state->have_caps || state->window_id == 0) {
    post_sink_failure(self, "preview window or negotiated caps are unavailable");
    return false;
  }
  initialize_xlib_once();
  state->display = XOpenDisplay(nullptr);
  if (!state->display) {
    post_sink_failure(self, "could not open the X11 display");
    return false;
  }
  XWindowAttributes attributes{};
  current_x_error_target = state;
  const Status attributes_status =
      XGetWindowAttributes(state->display, static_cast<Window>(state->window_id), &attributes);
  XSync(state->display, False);
  current_x_error_target = nullptr;
  if (!attributes_status || state->failed.load()) {
    post_sink_failure(self, "preview XID is invalid or stale");
    return false;
  }
  XVisualInfo visual_template{};
  visual_template.visualid = XVisualIDFromVisual(attributes.visual);
  int visual_count = 0;
  XVisualInfo* visual = XGetVisualInfo(state->display, VisualIDMask, &visual_template, &visual_count);
  if (!visual || visual_count != 1) {
    if (visual)
      XFree(visual);
    post_sink_failure(self, "preview XID visual is not GLX-compatible");
    return false;
  }
  int use_gl = 0;
  int rgba = 0;
  std::uint64_t error_serial = state->x_error_serial.load();
  current_x_error_target = state;
  const bool visual_compatible = glXGetConfig(state->display, visual, GLX_USE_GL, &use_gl) == 0 && use_gl != 0 &&
      glXGetConfig(state->display, visual, GLX_RGBA, &rgba) == 0 && rgba != 0;
  XSync(state->display, False);
  current_x_error_target = nullptr;
  if (!visual_compatible || state->x_error_serial.load() != error_serial) {
    XFree(visual);
    post_sink_failure(self, "preview XID visual does not support RGBA GLX rendering");
    return false;
  }
  error_serial = state->x_error_serial.load();
  current_x_error_target = state;
  state->context = glXCreateContext(state->display, visual, nullptr, True);
  XSync(state->display, False);
  current_x_error_target = nullptr;
  XFree(visual);
  if (!state->context || state->x_error_serial.load() != error_serial || !make_context_current(self)) {
    post_sink_failure(self, "could not create a GLX context for the preview XID");
    return false;
  }
  if (!cuda_succeeded(self, cudaSetDevice(state->gpu_id), "cudaSetDevice")) {
    release_context(state);
    return false;
  }
  unsigned int gl_device_count = 0;
  int gl_devices[16]{};
  if (!cuda_succeeded(
          self, cudaGLGetDevices(&gl_device_count, gl_devices, 16, cudaGLDeviceListAll), "cudaGLGetDevices")) {
    release_context(state);
    return false;
  }
  if (std::find(gl_devices, gl_devices + gl_device_count, static_cast<int>(state->gpu_id)) ==
      gl_devices + gl_device_count) {
    release_context(state);
    post_sink_failure(self, "preview X11/GLX GPU does not match the pipeline GPU");
    return false;
  }
  glGenTextures(1, &state->texture);
  glBindTexture(GL_TEXTURE_2D, state->texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(
      GL_TEXTURE_2D,
      0,
      GL_RGBA8,
      static_cast<GLsizei>(state->negotiated_width),
      static_cast<GLsizei>(state->negotiated_height),
      0,
      GL_RGBA,
      GL_UNSIGNED_BYTE,
      nullptr);
  if (glGetError() != GL_NO_ERROR) {
    release_context(state);
    post_sink_failure(self, "could not allocate the preview OpenGL texture");
    return false;
  }
  if (!cuda_succeeded(
          self,
          cudaGraphicsGLRegisterImage(
              &state->cuda_texture, state->texture, GL_TEXTURE_2D, cudaGraphicsRegisterFlagsWriteDiscard),
          "cudaGraphicsGLRegisterImage") ||
      !cuda_succeeded(
          self, cudaStreamCreateWithFlags(&state->cuda_stream, cudaStreamNonBlocking), "cudaStreamCreate") ||
      !cuda_succeeded(
          self, cudaEventCreateWithFlags(&state->copy_complete, cudaEventDisableTiming), "cudaEventCreate")) {
    release_context(state);
    return false;
  }
  release_context(state);
  post_preview_status(
      GST_ELEMENT(self),
      state->channel.c_str(),
      "initialized",
      state->generation.load(),
      "GPU-resident renderer ready");
  return true;
}

bool wait_for_copy_or_terminate(GstHmGpuPreviewSink* self) {
  RendererState* state = self->state;
  const auto deadline = std::chrono::steady_clock::now() + kGpuCompletionTimeout;
  for (;;) {
    const cudaError_t query = cudaEventQuery(state->copy_complete);
    if (query == cudaSuccess)
      return true;
    if (query != cudaErrorNotReady) {
      terminate_for_unsafe_cuda_state(self, "cudaEventQuery", query);
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      // CUDA may still be reading the borrowed NvBufSurface. Releasing the
      // GstBuffer or interop objects would corrupt the pool, so this declared
      // device-fatal path terminates without unwinding those resources.
      g_printerr("HSTREAM_PREVIEW_FATAL channel=%s message=GPU preview copy timed out\n", state->channel.c_str());
      std::_Exit(86);
    }
    std::this_thread::sleep_for(std::chrono::microseconds(100));
  }
}

OverlayColor overlay_color(const NvOSD_ColorParams& color) {
  return OverlayColor{
      static_cast<float>(color.red),
      static_cast<float>(color.green),
      static_cast<float>(color.blue),
      static_cast<float>(color.alpha),
  };
}

std::vector<hm::preview_overlay::Point> transform_points(
    const std::vector<hm::preview_overlay::Point>& points,
    const hm::preview_overlay::PlayCropperTransform* transform) {
  if (!transform)
    return points;
  std::vector<hm::preview_overlay::Point> transformed;
  transformed.reserve(points.size());
  for (const auto& point : points)
    transformed.push_back(hm::preview_overlay::metadata_to_output(*transform, point));
  return transformed;
}

void add_rect_paths(
    PreviewOverlays* overlays,
    float left,
    float top,
    float width,
    float height,
    float border_width,
    OverlayColor border,
    bool has_background,
    OverlayColor background,
    const hm::preview_overlay::PlayCropperTransform* transform) {
  if (!overlays || width <= 0.0F || height <= 0.0F)
    return;
  const std::vector<hm::preview_overlay::Point> raw = {
      {left, top},
      {left + width, top},
      {left + width, top + height},
      {left, top + height},
  };
  const auto points = transform_points(raw, transform);
  if (has_background && background.alpha > 0.0F)
    overlays->paths.push_back(OverlayPath{points, background, 1.0F, true, true});
  if (border_width > 0.0F && border.alpha > 0.0F)
    overlays->paths.push_back(OverlayPath{points, border, border_width, true, false});
}

PreviewOverlays collect_preview_overlays(GstHmGpuPreviewSink* self, GstBuffer* buffer) {
  RendererState* state = self->state;
  PreviewOverlays overlays;
  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(buffer);
  if (!batch_meta || !batch_meta->frame_meta_list)
    return overlays;
  auto* frame_meta = static_cast<NvDsFrameMeta*>(batch_meta->frame_meta_list->data);
  if (!frame_meta)
    return overlays;
  const auto* snapshot = hm::preview_overlay::find_overlay_snapshot_meta(frame_meta);
  const auto* attached_transform =
      state->channel == "program" ? hm::preview_overlay::find_playcropper_transform_meta(frame_meta) : nullptr;
  if (state->channel == "program" && attached_transform) {
    overlays.program_transform = *attached_transform;
    overlays.coordinate_width = attached_transform->output_width;
    overlays.coordinate_height = attached_transform->output_height;
  } else if (snapshot) {
    overlays.coordinate_width = snapshot->coordinate_width;
    overlays.coordinate_height = snapshot->coordinate_height;
  } else {
    overlays.coordinate_width = static_cast<float>(frame_meta->source_frame_width);
    overlays.coordinate_height = static_cast<float>(frame_meta->source_frame_height);
  }
  if (overlays.coordinate_width <= 0.0F || overlays.coordinate_height <= 0.0F) {
    overlays.coordinate_width = static_cast<float>(state->negotiated_width);
    overlays.coordinate_height = static_cast<float>(state->negotiated_height);
  }
  const auto* program_transform = overlays.program_transform ? &*overlays.program_transform : nullptr;

  if (state->show_player_tracking.load()) {
    auto add_player_rect = [&](const NvOSD_RectParams& rect,
                               const hm::preview_overlay::PlayCropperTransform* transform) {
      add_rect_paths(
          &overlays,
          rect.left,
          rect.top,
          rect.width,
          rect.height,
          std::max(2.0F, static_cast<float>(rect.border_width)),
          OverlayColor{0.0F, 1.0F, 1.0F, 0.95F},
          false,
          {},
          transform);
    };
    if (snapshot) {
      for (const NvOSD_RectParams& rect : snapshot->player_rects)
        add_player_rect(rect, program_transform);
    } else {
      const auto* object_transform =
          program_transform && !program_transform->object_meta_transformed ? program_transform : nullptr;
      for (NvDsMetaList* item = frame_meta->obj_meta_list; item; item = item->next) {
        auto* object_meta = static_cast<NvDsObjectMeta*>(item->data);
        if (!object_meta || object_meta->class_id != 0 || object_meta->object_id == UNTRACKED_OBJECT_ID)
          continue;
        add_player_rect(object_meta->rect_params, object_transform);
      }
    }
  }

  if (state->show_play_tracking.load()) {
    auto add_play_rect = [&](const NvOSD_RectParams& rect) {
      add_rect_paths(
          &overlays,
          rect.left,
          rect.top,
          rect.width,
          rect.height,
          static_cast<float>(rect.border_width),
          overlay_color(rect.border_color),
          rect.has_bg_color,
          overlay_color(rect.bg_color),
          program_transform);
    };
    auto add_play_line = [&](const NvOSD_LineParams& line) {
      overlays.paths.push_back(
          OverlayPath{
              transform_points(
                  {{static_cast<float>(line.x1), static_cast<float>(line.y1)},
                   {static_cast<float>(line.x2), static_cast<float>(line.y2)}},
                  program_transform),
              overlay_color(line.line_color),
              static_cast<float>(line.line_width),
              false,
              false,
          });
    };
    auto add_play_arrow = [&](const NvOSD_ArrowParams& arrow) {
      const OverlayColor color = overlay_color(arrow.arrow_color);
      const float width = static_cast<float>(arrow.arrow_width);
      overlays.paths.push_back(
          OverlayPath{
              transform_points(
                  {{static_cast<float>(arrow.x1), static_cast<float>(arrow.y1)},
                   {static_cast<float>(arrow.x2), static_cast<float>(arrow.y2)}},
                  program_transform),
              color,
              width,
              false,
              false,
          });
      for (const auto& triangle : hm::preview_overlay::arrow_head_triangles(
               {static_cast<float>(arrow.x1), static_cast<float>(arrow.y1)},
               {static_cast<float>(arrow.x2), static_cast<float>(arrow.y2)},
               width,
               arrow.arrow_head)) {
        overlays.paths.push_back(
            OverlayPath{
                transform_points(
                    std::vector<hm::preview_overlay::Point>(triangle.begin(), triangle.end()), program_transform),
                color,
                1.0F,
                true,
                true});
      }
    };
    auto add_play_circle = [&](const NvOSD_CircleParams& circle) {
      constexpr int kCircleSegments = 48;
      std::vector<hm::preview_overlay::Point> points;
      points.reserve(kCircleSegments);
      for (int segment = 0; segment < kCircleSegments; ++segment) {
        constexpr float kPi = 3.14159265358979323846F;
        const float angle = 2.0F * kPi * segment / kCircleSegments;
        points.push_back({
            static_cast<float>(circle.xc) + static_cast<float>(circle.radius) * std::cos(angle),
            static_cast<float>(circle.yc) + static_cast<float>(circle.radius) * std::sin(angle),
        });
      }
      points = transform_points(points, program_transform);
      if (circle.has_bg_color)
        overlays.paths.push_back(OverlayPath{points, overlay_color(circle.bg_color), 1.0F, true, true});
      overlays.paths.push_back(OverlayPath{points, overlay_color(circle.circle_color), 3.0F, true, false});
    };
    if (snapshot) {
      for (const NvOSD_RectParams& rect : snapshot->play_rects)
        add_play_rect(rect);
      for (const NvOSD_LineParams& line : snapshot->play_lines)
        add_play_line(line);
      for (const NvOSD_ArrowParams& arrow : snapshot->play_arrows)
        add_play_arrow(arrow);
      for (const NvOSD_CircleParams& circle : snapshot->play_circles)
        add_play_circle(circle);
    } else {
      for (NvDsMetaList* item = frame_meta->display_meta_list; item; item = item->next) {
        auto* display_meta = static_cast<NvDsDisplayMeta*>(item->data);
        if (!display_meta)
          continue;
        for (guint index = 0; index < display_meta->num_rects; ++index)
          add_play_rect(display_meta->rect_params[index]);
        for (guint index = 0; index < display_meta->num_lines; ++index)
          add_play_line(display_meta->line_params[index]);
        for (guint index = 0; index < display_meta->num_arrows; ++index)
          add_play_arrow(display_meta->arrow_params[index]);
        for (guint index = 0; index < display_meta->num_circles; ++index)
          add_play_circle(display_meta->circle_params[index]);
      }
    }
  }
  return overlays;
}

bool ensure_rink_mask_texture(GstHmGpuPreviewSink* self) {
  RendererState* state = self->state;
  if (!state->rink_mask_dirty)
    return state->rink_mask_texture != 0;
  const auto now = std::chrono::steady_clock::now();
  if (now < state->rink_mask_retry_after)
    return false;
  auto schedule_retry = [&] {
    state->rink_mask_consecutive_failures = std::min(state->rink_mask_consecutive_failures + 1U, 6U);
    const unsigned delay = hm::gpu_preview::rink_mask_retry_delay_seconds(state->rink_mask_consecutive_failures);
    state->rink_mask_retry_after = now + std::chrono::seconds(delay);
    return delay;
  };
  if (state->rink_mask_texture) {
    glDeleteTextures(1, &state->rink_mask_texture);
    state->rink_mask_texture = 0;
  }
  state->rink_mask_width = 0;
  state->rink_mask_height = 0;
  if (state->rink_mask_file.empty()) {
    state->rink_mask_dirty = false;
    state->rink_mask_consecutive_failures = 0;
    return false;
  }
  const hm::gpu_preview::RinkMaskLoadResult loaded = hm::gpu_preview::load_rink_mask_png(state->rink_mask_file);
  if (!loaded) {
    const unsigned retry_seconds = schedule_retry();
    if (!state->rink_mask_failure_reported) {
      g_printerr(
          "HSTREAM_PREVIEW_OVERLAY channel=%s rink-mask=%s status=%s retry=%us message=%s\n",
          state->channel.c_str(),
          state->rink_mask_file.c_str(),
          hm::gpu_preview::rink_mask_load_status_name(loaded.status),
          retry_seconds,
          loaded.message);
      state->rink_mask_failure_reported = true;
    }
    return false;
  }
  for (int stale_error = 0; stale_error < 16 && glGetError() != GL_NO_ERROR; ++stale_error) {
  }
  glGenTextures(1, &state->rink_mask_texture);
  glBindTexture(GL_TEXTURE_2D, state->rink_mask_texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
  constexpr GLfloat kTransparentBorder[4] = {0.0F, 0.0F, 0.0F, 0.0F};
  glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, kTransparentBorder);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(
      GL_TEXTURE_2D,
      0,
      GL_ALPHA8,
      static_cast<GLsizei>(loaded.image.width),
      static_cast<GLsizei>(loaded.image.height),
      0,
      GL_ALPHA,
      GL_UNSIGNED_BYTE,
      loaded.image.alpha.data());
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  if (state->rink_mask_texture == 0 || glGetError() != GL_NO_ERROR) {
    glDeleteTextures(1, &state->rink_mask_texture);
    state->rink_mask_texture = 0;
    const unsigned retry_seconds = schedule_retry();
    if (!state->rink_mask_failure_reported) {
      g_printerr(
          "HSTREAM_PREVIEW_OVERLAY channel=%s rink-mask=%s status=texture-upload-failed retry=%us\n",
          state->channel.c_str(),
          state->rink_mask_file.c_str(),
          retry_seconds);
      state->rink_mask_failure_reported = true;
    }
    return false;
  }
  state->rink_mask_width = loaded.image.width;
  state->rink_mask_height = loaded.image.height;
  state->rink_mask_dirty = false;
  state->rink_mask_failure_reported = false;
  state->rink_mask_consecutive_failures = 0;
  state->rink_mask_retry_after = {};
  g_print(
      "HSTREAM_PREVIEW_OVERLAY channel=%s rink-mask=%s status=loaded\n",
      state->channel.c_str(),
      state->rink_mask_file.c_str());
  return true;
}

void draw_rink_mask(GstHmGpuPreviewSink* self, const PreviewOverlays& overlays) {
  RendererState* state = self->state;
  if (!state->show_rink_mask.load() || !ensure_rink_mask_texture(self))
    return;
  std::array<hm::preview_overlay::Point, 4> texture_points = {
      hm::preview_overlay::Point{0.0F, overlays.coordinate_height},
      hm::preview_overlay::Point{overlays.coordinate_width, overlays.coordinate_height},
      hm::preview_overlay::Point{overlays.coordinate_width, 0.0F},
      hm::preview_overlay::Point{0.0F, 0.0F},
  };
  if (overlays.program_transform) {
    for (auto& point : texture_points)
      point = hm::preview_overlay::output_to_input(*overlays.program_transform, point);
  }
  const float source_width =
      overlays.program_transform ? overlays.program_transform->input_width : overlays.coordinate_width;
  const float source_height =
      overlays.program_transform ? overlays.program_transform->input_height : overlays.coordinate_height;
  if (source_width <= 0.0F || source_height <= 0.0F)
    return;
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, state->rink_mask_texture);
  glColor4f(0.0F, 1.0F, 0.0F, 0.10F);
  glBegin(GL_QUADS);
  constexpr std::array<std::array<float, 2>, 4> vertices = {
      std::array<float, 2>{-1.0F, -1.0F},
      std::array<float, 2>{1.0F, -1.0F},
      std::array<float, 2>{1.0F, 1.0F},
      std::array<float, 2>{-1.0F, 1.0F},
  };
  for (size_t index = 0; index < texture_points.size(); ++index) {
    glTexCoord2f(texture_points[index].x / source_width, texture_points[index].y / source_height);
    glVertex2f(vertices[index][0], vertices[index][1]);
  }
  glEnd();
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
}

void draw_overlay_paths(const PreviewOverlays& overlays) {
  if (overlays.coordinate_width <= 0.0F || overlays.coordinate_height <= 0.0F)
    return;
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  for (const OverlayPath& path : overlays.paths) {
    if (path.points.size() < (path.filled ? 3U : 2U))
      continue;
    glColor4f(path.color.red, path.color.green, path.color.blue, path.color.alpha);
    glLineWidth(std::max(1.0F, path.width));
    glBegin(path.filled ? GL_POLYGON : (path.closed ? GL_LINE_LOOP : GL_LINE_STRIP));
    for (const auto& point : path.points) {
      glVertex2f(
          -1.0F + 2.0F * point.x / overlays.coordinate_width, 1.0F - 2.0F * point.y / overlays.coordinate_height);
    }
    glEnd();
  }
  glLineWidth(1.0F);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
}

void draw_texture(GstHmGpuPreviewSink* self, const PreviewOverlays& overlays) {
  RendererState* state = self->state;
  XWindowAttributes attributes{};
  current_x_error_target = state;
  XGetWindowAttributes(state->display, static_cast<Window>(state->window_id), &attributes);
  XSync(state->display, False);
  current_x_error_target = nullptr;
  if (state->failed.load() || attributes.width <= 0 || attributes.height <= 0) {
    post_sink_failure(self, "preview window was destroyed or has invalid geometry");
    return;
  }

  const double source_width = state->source_width ? state->source_width : state->negotiated_width;
  const double source_height = state->source_height ? state->source_height : state->negotiated_height;
  const double source_aspect = source_width / source_height;
  const double window_aspect = static_cast<double>(attributes.width) / attributes.height;
  int viewport_width = attributes.width;
  int viewport_height = attributes.height;
  if (window_aspect > source_aspect)
    viewport_width = std::max(1, static_cast<int>(std::lround(attributes.height * source_aspect)));
  else
    viewport_height = std::max(1, static_cast<int>(std::lround(attributes.width / source_aspect)));
  const int viewport_x = (attributes.width - viewport_width) / 2;
  const int viewport_y = (attributes.height - viewport_height) / 2;

  glViewport(0, 0, attributes.width, attributes.height);
  glClearColor(0.0F, 0.0F, 0.0F, 1.0F);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(viewport_x, viewport_y, viewport_width, viewport_height);
  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, state->texture);
  glBegin(GL_QUADS);
  glTexCoord2f(0.0F, 1.0F);
  glVertex2f(-1.0F, -1.0F);
  glTexCoord2f(1.0F, 1.0F);
  glVertex2f(1.0F, -1.0F);
  glTexCoord2f(1.0F, 0.0F);
  glVertex2f(1.0F, 1.0F);
  glTexCoord2f(0.0F, 0.0F);
  glVertex2f(-1.0F, 1.0F);
  glEnd();
  draw_rink_mask(self, overlays);
  draw_overlay_paths(overlays);
  current_x_error_target = state;
  glXSwapBuffers(state->display, static_cast<GLXDrawable>(state->window_id));
  XSync(state->display, False);
  current_x_error_target = nullptr;
  if (state->failed.load())
    post_sink_failure(self, "preview window rejected OpenGL presentation");
}

gboolean preview_sink_start(GstBaseSink* base_sink) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(base_sink);
  self->state->stopping = false;
  return TRUE;
}

gboolean preview_sink_set_caps(GstBaseSink* base_sink, GstCaps* caps) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(base_sink);
  RendererState* state = self->state;
  if (!caps || gst_caps_get_size(caps) != 1) {
    post_sink_failure(self, "preview requires one fixed RGBA NVMM caps structure");
    return TRUE;
  }
  const GstCapsFeatures* features = gst_caps_get_features(caps, 0);
  GstVideoInfo info{};
  if (!features || !gst_caps_features_contains(features, "memory:NVMM") || !gst_video_info_from_caps(&info, caps) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGBA || GST_VIDEO_INFO_WIDTH(&info) <= 0 ||
      GST_VIDEO_INFO_HEIGHT(&info) <= 0) {
    post_sink_failure(self, "preview negotiated non-RGBA or non-NVMM video");
    return TRUE;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  state->video_info = info;
  state->negotiated_width = GST_VIDEO_INFO_WIDTH(&info);
  state->negotiated_height = GST_VIDEO_INFO_HEIGHT(&info);
  state->have_caps = true;
  return TRUE;
}

GstFlowReturn preview_sink_render(GstBaseSink* base_sink, GstBuffer* buffer) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(base_sink);
  RendererState* state = self->state;
  if (state->failed.load())
    return GST_FLOW_ERROR;
  if (state->stopping.load())
    return GST_FLOW_OK;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->failed.load())
    return GST_FLOW_ERROR;
  if (state->stopping.load())
    return GST_FLOW_OK;
  if (!cuda_succeeded(self, cudaSetDevice(state->gpu_id), "cudaSetDevice"))
    return GST_FLOW_ERROR;
  if (!initialize_renderer(self))
    return GST_FLOW_ERROR;
  GstMapInfo map{};
  if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    post_sink_failure(self, "could not inspect the NvBufSurface descriptor");
    return GST_FLOW_ERROR;
  }
  auto* surface = reinterpret_cast<NvBufSurface*>(map.data);
  bool valid = surface && surface->batchSize >= 1 && surface->numFilled == 1 && surface->surfaceList &&
      surface->gpuId == state->gpu_id && surface->memType == NVBUF_MEM_CUDA_DEVICE;
  NvBufSurfaceParams* params = valid ? &surface->surfaceList[0] : nullptr;
  valid = valid && params->dataPtr && params->layout == NVBUF_LAYOUT_PITCH &&
      params->colorFormat == NVBUF_COLOR_FORMAT_RGBA && params->width == state->negotiated_width &&
      params->height == state->negotiated_height && params->pitch >= state->negotiated_width * 4U;
  if (!valid) {
    gst_buffer_unmap(buffer, &map);
    post_sink_failure(self, "preview received an invalid or non-device RGBA NvBufSurface");
    return GST_FLOW_ERROR;
  }
  const PreviewOverlays overlays = collect_preview_overlays(self, buffer);
  if (!make_context_current(self)) {
    gst_buffer_unmap(buffer, &map);
    return GST_FLOW_ERROR;
  }
  const cudaError_t map_result = cudaGraphicsMapResources(1, &state->cuda_texture, state->cuda_stream);
  if (!cuda_succeeded(self, map_result, "cudaGraphicsMapResources")) {
    gst_buffer_unmap(buffer, &map);
    release_context(state);
    return GST_FLOW_ERROR;
  }
  cudaArray_t texture_array = nullptr;
  cudaError_t result = cudaGraphicsSubResourceGetMappedArray(&texture_array, state->cuda_texture, 0, 0);
  if (result != cudaSuccess)
    terminate_for_unsafe_cuda_state(self, "cudaGraphicsSubResourceGetMappedArray", result);
  result = cudaMemcpy2DToArrayAsync(
      texture_array,
      0,
      0,
      params->dataPtr,
      params->pitch,
      static_cast<size_t>(state->negotiated_width) * 4,
      state->negotiated_height,
      cudaMemcpyDeviceToDevice,
      state->cuda_stream);
  if (result != cudaSuccess)
    terminate_for_unsafe_cuda_state(self, "cudaMemcpy2DToArrayAsync", result);
  result = cudaGraphicsUnmapResources(1, &state->cuda_texture, state->cuda_stream);
  if (result != cudaSuccess)
    terminate_for_unsafe_cuda_state(self, "cudaGraphicsUnmapResources", result);
  result = cudaEventRecord(state->copy_complete, state->cuda_stream);
  if (result != cudaSuccess)
    terminate_for_unsafe_cuda_state(self, "cudaEventRecord", result);
  wait_for_copy_or_terminate(self);
  // The explicit completion event above is the source NvBufSurface lifetime
  // barrier. No pixel plane has been mapped to host memory.
  gst_buffer_unmap(buffer, &map);
  if (!state->failed.load()) {
    draw_texture(self, overlays);
    const guint64 generation = state->generation.load();
    if (!state->failed.load() && state->ready_generation != generation) {
      state->ready_generation = generation;
      post_preview_status(GST_ELEMENT(self), state->channel.c_str(), "ready", generation, "first GPU frame presented");
    }
  }
  release_context(state);
  return state->failed.load() ? GST_FLOW_ERROR : GST_FLOW_OK;
}

gboolean preview_sink_unlock(GstBaseSink* base_sink) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(base_sink);
  self->state->stopping = true;
  return TRUE;
}

gboolean preview_sink_unlock_stop(GstBaseSink* base_sink) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(base_sink);
  self->state->stopping = false;
  return TRUE;
}

bool destroy_renderer_locked(GstHmGpuPreviewSink* self) {
  RendererState* state = self->state;
  if (state->context) {
    if (!make_context_current(self))
      return false;
    glFinish();
    if (!cuda_succeeded(self, cudaSetDevice(state->gpu_id), "cudaSetDevice during preview cleanup")) {
      release_context(state);
      return false;
    }
    if (state->cuda_texture &&
        !cuda_succeeded(self, cudaGraphicsUnregisterResource(state->cuda_texture), "cudaGraphicsUnregisterResource")) {
      release_context(state);
      return false;
    }
    state->cuda_texture = nullptr;
    if (state->copy_complete && !cuda_succeeded(self, cudaEventDestroy(state->copy_complete), "cudaEventDestroy")) {
      release_context(state);
      return false;
    }
    state->copy_complete = nullptr;
    if (state->cuda_stream && !cuda_succeeded(self, cudaStreamDestroy(state->cuda_stream), "cudaStreamDestroy")) {
      release_context(state);
      return false;
    }
    state->cuda_stream = nullptr;
    if (state->texture)
      glDeleteTextures(1, &state->texture);
    state->texture = 0;
    if (state->rink_mask_texture)
      glDeleteTextures(1, &state->rink_mask_texture);
    state->rink_mask_texture = 0;
    release_context(state);
  }
  if (state->display && state->context)
    glXDestroyContext(state->display, state->context);
  state->context = nullptr;
  if (state->display)
    XCloseDisplay(state->display);
  state->display = nullptr;
  return true;
}

gboolean preview_sink_stop(GstBaseSink* base_sink) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(base_sink);
  RendererState* state = self->state;
  state->stopping = true;
  std::lock_guard<std::mutex> lock(state->mutex);
  destroy_renderer_locked(self);
  return TRUE;
}

void preview_sink_set_property(GObject* object, guint property_id, const GValue* value, GParamSpec* spec) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(object);
  RendererState* state = self->state;
  if (property_id == kSinkPropertyGeneration) {
    state->generation = g_value_get_uint64(value);
    return;
  }
  if (property_id == kSinkPropertyShowPlayerTracking) {
    state->show_player_tracking = g_value_get_boolean(value);
    return;
  }
  if (property_id == kSinkPropertyShowPlayTracking) {
    state->show_play_tracking = g_value_get_boolean(value);
    return;
  }
  if (property_id == kSinkPropertyShowRinkMask) {
    state->show_rink_mask = g_value_get_boolean(value);
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  switch (property_id) {
    case kSinkPropertyWindowId:
      state->window_id = g_value_get_uint64(value);
      break;
    case kSinkPropertyGpuId:
      state->gpu_id = g_value_get_uint(value);
      break;
    case kSinkPropertyChannel:
      state->channel = g_value_get_string(value) ? g_value_get_string(value) : "unknown";
      break;
    case kSinkPropertySourceWidth:
      state->source_width = g_value_get_uint(value);
      break;
    case kSinkPropertySourceHeight:
      state->source_height = g_value_get_uint(value);
      break;
    case kSinkPropertyRinkMaskFile:
      state->rink_mask_file = g_value_get_string(value) ? g_value_get_string(value) : "";
      state->rink_mask_dirty = true;
      state->rink_mask_failure_reported = false;
      state->rink_mask_consecutive_failures = 0;
      state->rink_mask_retry_after = {};
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
  }
}

void preview_sink_get_property(GObject* object, guint property_id, GValue* value, GParamSpec* spec) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(object);
  RendererState* state = self->state;
  if (property_id == kSinkPropertyGeneration) {
    g_value_set_uint64(value, state->generation.load());
    return;
  }
  if (property_id == kSinkPropertyShowPlayerTracking) {
    g_value_set_boolean(value, state->show_player_tracking.load());
    return;
  }
  if (property_id == kSinkPropertyShowPlayTracking) {
    g_value_set_boolean(value, state->show_play_tracking.load());
    return;
  }
  if (property_id == kSinkPropertyShowRinkMask) {
    g_value_set_boolean(value, state->show_rink_mask.load());
    return;
  }
  std::lock_guard<std::mutex> lock(state->mutex);
  switch (property_id) {
    case kSinkPropertyWindowId:
      g_value_set_uint64(value, state->window_id);
      break;
    case kSinkPropertyGpuId:
      g_value_set_uint(value, state->gpu_id);
      break;
    case kSinkPropertyChannel:
      g_value_set_string(value, state->channel.c_str());
      break;
    case kSinkPropertySourceWidth:
      g_value_set_uint(value, state->source_width);
      break;
    case kSinkPropertySourceHeight:
      g_value_set_uint(value, state->source_height);
      break;
    case kSinkPropertyRinkMaskFile:
      g_value_set_string(value, state->rink_mask_file.c_str());
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property_id, spec);
  }
}

void preview_sink_finalize(GObject* object) {
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(object);
  {
    std::lock_guard<std::mutex> lock(self->state->mutex);
    destroy_renderer_locked(self);
  }
  delete self->state;
  self->state = nullptr;
  G_OBJECT_CLASS(gst_hm_gpu_preview_sink_parent_class)->finalize(object);
}

void gst_hm_gpu_preview_sink_class_init(GstHmGpuPreviewSinkClass* klass) {
  auto* object_class = G_OBJECT_CLASS(klass);
  auto* element_class = GST_ELEMENT_CLASS(klass);
  auto* base_sink_class = GST_BASE_SINK_CLASS(klass);
  object_class->set_property = preview_sink_set_property;
  object_class->get_property = preview_sink_get_property;
  object_class->finalize = preview_sink_finalize;
  g_object_class_install_property(
      object_class,
      kSinkPropertyWindowId,
      g_param_spec_uint64("window-id", "Window ID", "Foreign X11 render target", 0, G_MAXUINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyGpuId,
      g_param_spec_uint("gpu-id", "GPU ID", "CUDA/GL device", 0, G_MAXUINT, 0, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyChannel,
      g_param_spec_string("channel", "Channel", "Logical preview channel", "unknown", G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyGeneration,
      g_param_spec_uint64("generation", "Generation", "Activation generation", 0, G_MAXUINT64, 0, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertySourceWidth,
      g_param_spec_uint(
          "source-width", "Source width", "Original aspect-ratio width", 0, G_MAXUINT, 0, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertySourceHeight,
      g_param_spec_uint(
          "source-height", "Source height", "Original aspect-ratio height", 0, G_MAXUINT, 0, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyShowPlayerTracking,
      g_param_spec_boolean(
          "show-player-tracking",
          "Show player tracking",
          "Draw tracked-player boxes in the GPU preview",
          FALSE,
          G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyShowPlayTracking,
      g_param_spec_boolean(
          "show-play-tracking",
          "Show play tracking",
          "Draw play-tracker state geometry in the GPU preview",
          FALSE,
          G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyShowRinkMask,
      g_param_spec_boolean(
          "show-rink-mask", "Show rink mask", "Composite the rink mask in the GPU preview", FALSE, G_PARAM_READWRITE));
  g_object_class_install_property(
      object_class,
      kSinkPropertyRinkMaskFile,
      g_param_spec_string(
          "rink-mask-file", "Rink mask file", "Saved stitched-canvas rink mask", "", G_PARAM_READWRITE));
  GstCaps* caps = gst_caps_from_string(
      "video/x-raw(memory:NVMM),format=(string)RGBA,width=(int)[1,4096],height=(int)[1,2160],"
      "framerate=(fraction)[0/1,120/1]");
  gst_element_class_add_pad_template(element_class, gst_pad_template_new("sink", GST_PAD_SINK, GST_PAD_ALWAYS, caps));
  gst_caps_unref(caps);
  gst_element_class_set_static_metadata(
      element_class,
      "HStream CUDA/OpenGL preview sink",
      "Sink/Video",
      "Renders CUDA-device NVMM into a foreign X11 window without device-to-host copies",
      "HStream");
  base_sink_class->start = preview_sink_start;
  base_sink_class->stop = preview_sink_stop;
  base_sink_class->set_caps = preview_sink_set_caps;
  base_sink_class->render = preview_sink_render;
  base_sink_class->unlock = preview_sink_unlock;
  base_sink_class->unlock_stop = preview_sink_unlock_stop;
}

void gst_hm_gpu_preview_sink_init(GstHmGpuPreviewSink* self) {
  self->state = new RendererState();
  gst_base_sink_set_sync(GST_BASE_SINK(self), FALSE);
  gst_base_sink_set_async_enabled(GST_BASE_SINK(self), FALSE);
  gst_base_sink_set_qos_enabled(GST_BASE_SINK(self), FALSE);
  gst_base_sink_set_last_sample_enabled(GST_BASE_SINK(self), FALSE);
}

} // namespace

#endif // defined(__x86_64__)

namespace hm::gpu_preview {

void initialize_process() {
#if defined(__x86_64__)
  initialize_xlib_once();
#endif
}

bool renderer_available() {
#if defined(__x86_64__)
  return true;
#else
  return false;
#endif
}

bool register_elements() {
  initialize_process();
  static gsize registration_result = 0;
  if (g_once_init_enter(&registration_result)) {
    gboolean registered =
        gst_element_register(nullptr, "hmpreviewisolation", GST_RANK_NONE, gst_hm_preview_isolation_get_type());
#if defined(__x86_64__)
    registered = registered &&
        gst_element_register(nullptr, "hmgpupreviewsink", GST_RANK_NONE, gst_hm_gpu_preview_sink_get_type());
#endif
    g_once_init_leave(&registration_result, registered ? 1 : 2);
  }
  return registration_result == 1;
}

void set_isolation_active(GstElement* isolation, bool active, std::uint64_t generation) {
  if (!isolation)
    return;
  g_object_set(
      G_OBJECT(isolation), "generation", static_cast<guint64>(generation), "active", active ? TRUE : FALSE, nullptr);
}

void set_isolation_generation(GstElement* isolation, std::uint64_t generation) {
  if (!isolation)
    return;
  // Generation is atomic and does not participate in the flow barrier. A
  // same-channel readiness retry must not wait behind the active buffer it is
  // trying to observe.
  g_object_set(G_OBJECT(isolation), "generation", static_cast<guint64>(generation), nullptr);
}

void set_isolation_failure_peer(GstElement* isolation, GstElement* ingress) {
  if (!isolation || !G_TYPE_CHECK_INSTANCE_TYPE(isolation, gst_hm_preview_isolation_get_type()))
    return;
  auto* self = reinterpret_cast<GstHmPreviewIsolation*>(isolation);
  g_weak_ref_set(&self->state->failure_peer, ingress ? G_OBJECT(ingress) : nullptr);
}

void set_renderer_generation(GstElement* sink, std::uint64_t generation) {
#if defined(__x86_64__)
  if (!sink || !G_TYPE_CHECK_INSTANCE_TYPE(sink, gst_hm_gpu_preview_sink_get_type()))
    return;
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(sink);
  // Generation is status metadata. Updating it must never wait for a frame
  // currently rendering under the renderer mutex.
  self->state->generation = generation;
#else
  (void)sink;
  (void)generation;
#endif
}

bool isolation_active(GstElement* isolation) {
  if (!isolation)
    return false;
  gboolean active = FALSE;
  g_object_get(G_OBJECT(isolation), "active", &active, nullptr);
  return active;
}

void set_source_geometry(GstElement* sink, unsigned width, unsigned height) {
  if (!sink)
    return;
  g_object_set(G_OBJECT(sink), "source-width", width, "source-height", height, nullptr);
}

bool quiesce(GstElement* sink, std::uint64_t generation) {
#if defined(__x86_64__)
  if (!sink || !G_TYPE_CHECK_INSTANCE_TYPE(sink, gst_hm_gpu_preview_sink_get_type()))
    return false;
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(sink);
  RendererState* state = self->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  const bool released = destroy_renderer_locked(self);
  post_preview_status(
      GST_ELEMENT(self),
      state->channel.c_str(),
      "deactivated",
      generation,
      released ? "GPU renderer released and quiesced" : "GPU renderer quiesced; cleanup deferred");
  return true;
#else
  (void)sink;
  (void)generation;
  return false;
#endif
}

std::pair<unsigned, unsigned> bounded_capture_dimensions(unsigned width, unsigned height) {
  if (width == 0 || height == 0)
    return {0, 0};
  constexpr std::uint64_t kBytesPerPixel = 4;
  constexpr std::uint64_t kMaximumPixels = kMaximumPresentedFrameCaptureBytes / kBytesPerPixel;
  const std::uint64_t source_pixels = static_cast<std::uint64_t>(width) * height;
  if (source_pixels <= kMaximumPixels)
    return {width, height};
  const long double scale = std::sqrt(static_cast<long double>(kMaximumPixels) / source_pixels);
  unsigned bounded_width = std::max(1U, static_cast<unsigned>(std::floor(width * scale)));
  unsigned bounded_height = std::max(1U, static_cast<unsigned>(std::floor(height * scale)));
  if (static_cast<std::uint64_t>(bounded_width) * bounded_height > kMaximumPixels) {
    if (bounded_width >= bounded_height) {
      bounded_width = std::max(1U, static_cast<unsigned>(kMaximumPixels / bounded_height));
    } else {
      bounded_height = std::max(1U, static_cast<unsigned>(kMaximumPixels / bounded_width));
    }
  }
  return {bounded_width, bounded_height};
}

bool capture_presented_frame(
    GstElement* sink,
    std::vector<std::uint8_t>* rgba,
    unsigned* width,
    unsigned* height,
    std::string* error) {
#if defined(__x86_64__)
  if (!sink || !rgba || !width || !height || !G_TYPE_CHECK_INSTANCE_TYPE(sink, gst_hm_gpu_preview_sink_get_type())) {
    if (error)
      *error = "GPU preview renderer is unavailable";
    return false;
  }
  auto* self = reinterpret_cast<GstHmGpuPreviewSink*>(sink);
  RendererState* state = self->state;
  std::lock_guard<std::mutex> lock(state->mutex);
  if (state->failed.load() || !state->context || !state->texture || state->negotiated_width == 0 ||
      state->negotiated_height == 0) {
    if (error)
      *error = "GPU preview has not presented a frame";
    return false;
  }
  if (!make_context_current(self)) {
    if (error)
      *error = "could not activate the preview OpenGL context";
    return false;
  }
  XWindowAttributes attributes{};
  const std::uint64_t error_serial = state->x_error_serial.load();
  current_x_error_target = state;
  const Status attributes_status =
      XGetWindowAttributes(state->display, static_cast<Window>(state->window_id), &attributes);
  XSync(state->display, False);
  current_x_error_target = nullptr;
  constexpr int kMaximumCaptureDimension = 16384;
  if (!attributes_status || state->x_error_serial.load() != error_serial || attributes.width <= 0 ||
      attributes.height <= 0 || attributes.width > kMaximumCaptureDimension ||
      attributes.height > kMaximumCaptureDimension) {
    release_context(state);
    if (error)
      *error = "preview window geometry is unavailable";
    return false;
  }
  const auto [capture_width, capture_height] =
      bounded_capture_dimensions(static_cast<unsigned>(attributes.width), static_cast<unsigned>(attributes.height));
  const size_t row_bytes = static_cast<size_t>(capture_width) * 4U;
  const size_t byte_count = row_bytes * static_cast<size_t>(capture_height);
  rgba->resize(byte_count);
  for (int stale_error = 0; stale_error < 16 && glGetError() != GL_NO_ERROR; ++stale_error) {
  }
  GLuint capture_texture = 0;
  GLuint capture_framebuffer = 0;
  bool capture_target_ready = true;
  const bool downsample = capture_width != static_cast<unsigned>(attributes.width) ||
      capture_height != static_cast<unsigned>(attributes.height);
  if (downsample) {
    glGenTextures(1, &capture_texture);
    glBindTexture(GL_TEXTURE_2D, capture_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA8,
        static_cast<GLsizei>(capture_width),
        static_cast<GLsizei>(capture_height),
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        nullptr);
    glGenFramebuffers(1, &capture_framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, capture_framebuffer);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, capture_texture, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_FRONT);
    if (glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      glBlitFramebuffer(
          0,
          0,
          attributes.width,
          attributes.height,
          0,
          0,
          static_cast<GLint>(capture_width),
          static_cast<GLint>(capture_height),
          GL_COLOR_BUFFER_BIT,
          GL_LINEAR);
      glBindFramebuffer(GL_READ_FRAMEBUFFER, capture_framebuffer);
      glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
      capture_target_ready = false;
    }
  } else {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glReadBuffer(GL_FRONT);
  }
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  if (capture_target_ready) {
    glReadPixels(
        0,
        0,
        static_cast<GLsizei>(capture_width),
        static_cast<GLsizei>(capture_height),
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        rgba->data());
  }
  glFinish();
  const GLenum gl_error = glGetError();
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  if (capture_framebuffer)
    glDeleteFramebuffers(1, &capture_framebuffer);
  if (capture_texture)
    glDeleteTextures(1, &capture_texture);
  release_context(state);
  if (!capture_target_ready || gl_error != GL_NO_ERROR) {
    rgba->clear();
    if (error)
      *error = "OpenGL presented-frame readback failed";
    return false;
  }
  for (unsigned top = 0, bottom = capture_height - 1; top < bottom; ++top, --bottom) {
    auto* top_row = rgba->data() + static_cast<size_t>(top) * row_bytes;
    auto* bottom_row = rgba->data() + static_cast<size_t>(bottom) * row_bytes;
    std::swap_ranges(top_row, top_row + row_bytes, bottom_row);
  }
  *width = capture_width;
  *height = capture_height;
  if (error)
    error->clear();
  return true;
#else
  (void)sink;
  (void)rgba;
  (void)width;
  (void)height;
  if (error)
    *error = "GPU preview renderer is unavailable on this platform";
  return false;
#endif
}

} // namespace hm::gpu_preview
