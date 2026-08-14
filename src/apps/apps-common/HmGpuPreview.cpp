#include "HmGpuPreview.h"

#include <gst/base/gstbasesink.h>
#include <gst/video/video.h>

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
#include <nvbufsurface.h>

#include <algorithm>
#include <chrono>
#include <thread>
#include <vector>

namespace {

constexpr auto kGpuCompletionTimeout = std::chrono::seconds(5);

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

void draw_texture(GstHmGpuPreviewSink* self) {
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
    draw_texture(self);
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
  const size_t byte_count = static_cast<size_t>(state->negotiated_width) * state->negotiated_height * 4U;
  rgba->resize(byte_count);
  glBindTexture(GL_TEXTURE_2D, state->texture);
  glPixelStorei(GL_PACK_ALIGNMENT, 1);
  glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->data());
  glFinish();
  const GLenum gl_error = glGetError();
  release_context(state);
  if (gl_error != GL_NO_ERROR) {
    rgba->clear();
    if (error)
      *error = "OpenGL diagnostic texture readback failed";
    return false;
  }
  *width = state->negotiated_width;
  *height = state->negotiated_height;
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
