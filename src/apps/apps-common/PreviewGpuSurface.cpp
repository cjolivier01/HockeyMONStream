#include "PreviewGpuSurface.h"

#include "SelectedPreviewStream.h"

#include <nvbufsurface.h>
#include <nvbufsurftransform.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <new>
#include <utility>
#include <vector>

namespace {
struct Surface {
  NvBufSurface* value{nullptr};
  ~Surface() {
    if (value) {
      cudaSetDevice(value->gpuId);
      NvBufSurfaceDestroy(value);
    }
  }
};

struct State {
  hm::preview::GpuSurfaceConfig config;
  hm::preview::SurfacePainter painter;
  std::vector<std::shared_ptr<Surface>> surfaces;
  cudaStream_t stream{nullptr};
  GstClockTime last_slot{GST_CLOCK_TIME_NONE};
  bool failed{false};
  ~State() {
    if (stream) {
      cudaSetDevice(config.gpu_id);
      cudaStreamSynchronize(stream);
      cudaStreamDestroy(stream);
    }
  }
};

typedef struct _GstHmPreviewGpuSurface {
  GstElement parent;
  GstPad *sink, *src;
  State* state;
} GstHmPreviewGpuSurface;
typedef struct _GstHmPreviewGpuSurfaceClass {
  GstElementClass parent_class;
} GstHmPreviewGpuSurfaceClass;
G_DEFINE_TYPE(GstHmPreviewGpuSurface, gst_hm_preview_gpu_surface, GST_TYPE_ELEMENT)

GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE("sink", GST_PAD_SINK, GST_PAD_ALWAYS, GST_STATIC_CAPS("video/x-raw(memory:NVMM)"));
GstStaticPadTemplate source_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS("video/x-raw(memory:NVMM),format=RGBA"));

GstCaps* output_caps(const State& state) {
  auto* caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "RGBA",
      "width",
      G_TYPE_INT,
      state.config.width,
      "height",
      G_TYPE_INT,
      state.config.height,
      "framerate",
      GST_TYPE_FRACTION,
      state.config.rate_numerator,
      state.config.rate_denominator,
      "pixel-aspect-ratio",
      GST_TYPE_FRACTION,
      1,
      1,
      nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
  return caps;
}

bool initialize(State& state) {
  if (state.stream)
    return true;
  if (cudaSetDevice(state.config.gpu_id) != cudaSuccess ||
      cudaStreamCreateWithFlags(&state.stream, cudaStreamNonBlocking) != cudaSuccess)
    return false;
  for (std::uint32_t index = 0; index < state.config.pool_frames; ++index) {
    auto surface = std::make_shared<Surface>();
    NvBufSurfaceCreateParams parameters{};
    parameters.gpuId = state.config.gpu_id;
    parameters.width = state.config.width;
    parameters.height = state.config.height;
    parameters.layout = NVBUF_LAYOUT_PITCH;
    parameters.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    parameters.memType = NVBUF_MEM_CUDA_DEVICE;
    if (NvBufSurfaceCreate(&surface->value, 1, &parameters) != 0)
      return false;
    surface->value->numFilled = 1;
    state.surfaces.push_back(std::move(surface));
  }
  return true;
}

bool render(State& state, GstBuffer* input, Surface& output) {
  if (cudaSetDevice(state.config.gpu_id) != cudaSuccess)
    return false;
  GstMapInfo map{};
  // NVMM stores a CPU-readable descriptor, not raw frame bytes, in GstMemory.
  // No NvBufSurfaceMap/SyncForCpu or D2H operation is used here.
  if (!gst_buffer_map(input, &map, GST_MAP_READ))
    return false;
  auto* source = map.size >= sizeof(NvBufSurface) ? reinterpret_cast<NvBufSurface*>(map.data) : nullptr;
  const bool valid = source && source->surfaceList && source->numFilled == 1 && source->gpuId == state.config.gpu_id &&
      source->memType == NVBUF_MEM_CUDA_DEVICE && source->surfaceList[0].width && source->surfaceList[0].height &&
      source->surfaceList[0].dataPtr;
  bool rendered = false;
  if (valid) {
    const auto& in = source->surfaceList[0];
    auto& out = output.value->surfaceList[0];
    const double scale = std::min(double(out.width) / in.width, double(out.height) / in.height);
    const auto width = std::max(1u, std::min(out.width, static_cast<unsigned>(std::lround(in.width * scale))));
    const auto height = std::max(1u, std::min(out.height, static_cast<unsigned>(std::lround(in.height * scale))));
    NvBufSurfTransformRect source_rect{};
    source_rect.width = in.width;
    source_rect.height = in.height;
    NvBufSurfTransformRect destination_rect{};
    destination_rect.top = (out.height - height) / 2;
    destination_rect.left = (out.width - width) / 2;
    destination_rect.width = width;
    destination_rect.height = height;
    NvBufSurfTransformConfigParams session{};
    session.compute_mode = NvBufSurfTransformCompute_GPU;
    session.gpu_id = state.config.gpu_id;
    session.cuda_stream = state.stream;
    NvBufSurfTransformParams transform{};
    transform.transform_flag = NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST |
        NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_ALLOW_ODD_CROP;
    transform.transform_filter = NvBufSurfTransformInter_Bilinear;
    transform.src_rect = &source_rect;
    transform.dst_rect = &destination_rect;
    // Clear the full owned surface on the same GPU stream before fitting the
    // source. A previous route's pixels must never remain in letterbox bars.
    rendered = cudaMemset2DAsync(out.dataPtr, out.pitch, 0, out.width * 4, out.height, state.stream) == cudaSuccess &&
        NvBufSurfTransformSetSessionParams(&session) == NvBufSurfTransformError_Success &&
        NvBufSurfTransform(source, output.value, &transform) == NvBufSurfTransformError_Success;
    if (rendered && state.painter) {
      try {
        rendered = state.painter(
            input,
            {
                out.dataPtr,
                out.pitch,
                out.width,
                out.height,
                {destination_rect.left, destination_rect.top, width, height},
                state.stream,
            });
      } catch (...) {
        rendered = false;
      }
    }
    // The next converter/encoder owns an independent stream. Complete our GPU
    // writes before releasing input or exposing the output to that consumer.
    rendered = cudaStreamSynchronize(state.stream) == cudaSuccess && rendered;
  }
  gst_buffer_unmap(input, &map);
  return rendered;
}

GstFlowReturn chain(GstPad*, GstObject* parent, GstBuffer* input) noexcept {
  GstBuffer* output = nullptr;
  try {
    auto* self = reinterpret_cast<GstHmPreviewGpuSurface*>(parent);
    auto& state = *self->state;
    if (state.failed || !hm::preview::frame_identity(input) || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(input))) {
      gst_buffer_unref(input);
      return GST_FLOW_ERROR;
    }
    const auto slot = gst_util_uint64_scale(
        GST_BUFFER_PTS(input), state.config.rate_numerator, GST_SECOND * state.config.rate_denominator);
    if (state.last_slot == slot) {
      gst_buffer_unref(input);
      return GST_FLOW_OK;
    }
    if (!initialize(state)) {
      state.failed = true;
      gst_buffer_unref(input);
      return GST_FLOW_ERROR;
    }
    std::shared_ptr<Surface> surface;
    for (const auto& candidate : state.surfaces)
      if (candidate.use_count() == 1) {
        surface = candidate;
        break;
      }
    if (!surface) {
      gst_buffer_unref(input);
      return GST_FLOW_OK;
    }
    if (!render(state, input, *surface)) {
      state.failed = true;
      gst_buffer_unref(input);
      return GST_FLOW_ERROR;
    }
    auto hold = std::make_unique<std::shared_ptr<Surface>>(surface);
    output = gst_buffer_new_wrapped_full(
        GST_MEMORY_FLAG_READONLY,
        surface->value,
        sizeof(NvBufSurface),
        0,
        sizeof(NvBufSurface),
        hold.get(),
        [](gpointer data) { delete static_cast<std::shared_ptr<Surface>*>(data); });
    if (!output) {
      gst_buffer_unref(input);
      return GST_FLOW_ERROR;
    }
    hold.release();
    if (!hm::preview::copy_frame_identity(output, input)) {
      gst_buffer_unref(input);
      gst_buffer_unref(output);
      return GST_FLOW_ERROR;
    }
    GST_BUFFER_PTS(output) = GST_BUFFER_PTS(input);
    GST_BUFFER_DTS(output) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(output) =
        gst_util_uint64_scale(GST_SECOND, state.config.rate_denominator, state.config.rate_numerator);
    state.last_slot = slot;
    gst_buffer_unref(input);
    input = nullptr;
    auto* admitted = output;
    output = nullptr;
    return gst_pad_push(self->src, admitted);
  } catch (...) {
    if (input)
      gst_buffer_unref(input);
    if (output)
      gst_buffer_unref(output);
    return GST_FLOW_ERROR;
  }
}

gboolean event(GstPad*, GstObject* parent, GstEvent* incoming) noexcept {
  auto* self = reinterpret_cast<GstHmPreviewGpuSurface*>(parent);
  if (GST_EVENT_TYPE(incoming) == GST_EVENT_CAPS) {
    GstCaps* source = nullptr;
    gst_event_parse_caps(incoming, &source);
    const bool valid = source && gst_caps_is_fixed(source) && gst_caps_get_size(source) == 1 &&
        gst_structure_has_name(gst_caps_get_structure(source, 0), "video/x-raw") &&
        !gst_caps_features_is_any(gst_caps_get_features(source, 0)) &&
        gst_caps_features_contains(gst_caps_get_features(source, 0), "memory:NVMM");
    if (!valid) {
      self->state->failed = true;
      gst_event_unref(incoming);
      return FALSE;
    }
    GstCaps* caps = output_caps(*self->state);
    GstEvent* transformed = gst_event_new_caps(caps);
    gst_caps_unref(caps);
    gst_event_unref(incoming);
    return gst_pad_push_event(self->src, transformed);
  }
  return gst_pad_push_event(self->src, incoming);
}

gboolean query(GstPad* pad, GstObject* parent, GstQuery* incoming) noexcept {
  auto* self = reinterpret_cast<GstHmPreviewGpuSurface*>(parent);
  if (GST_QUERY_TYPE(incoming) == GST_QUERY_CAPS) {
    GstCaps* filter = nullptr;
    gst_query_parse_caps(incoming, &filter);
    GstCaps* caps = pad == self->src ? output_caps(*self->state) : gst_static_pad_template_get_caps(&sink_template);
    GstCaps* result = filter ? gst_caps_intersect_full(filter, caps, GST_CAPS_INTERSECT_FIRST) : gst_caps_ref(caps);
    gst_query_set_caps_result(incoming, result);
    gst_caps_unref(result);
    gst_caps_unref(caps);
    return TRUE;
  }
  // Input allocations belong to the processing graph. The output surfaces are
  // privately owned here, so downstream's pool requirements cannot leak through.
  if (GST_QUERY_TYPE(incoming) == GST_QUERY_ALLOCATION)
    return FALSE;
  return gst_pad_query_default(pad, parent, incoming);
}

void finalize(GObject* object) noexcept {
  auto* self = reinterpret_cast<GstHmPreviewGpuSurface*>(object);
  delete self->state;
  G_OBJECT_CLASS(gst_hm_preview_gpu_surface_parent_class)->finalize(object);
}

void gst_hm_preview_gpu_surface_class_init(GstHmPreviewGpuSurfaceClass* klass) {
  G_OBJECT_CLASS(klass)->finalize = finalize;
  auto* element = GST_ELEMENT_CLASS(klass);
  gst_element_class_set_static_metadata(
      element, "HStream owned GPU preview surface", "Filter/Video", "Bounded GPU letterbox and drawing", "HStream");
  gst_element_class_add_static_pad_template(element, &sink_template);
  gst_element_class_add_static_pad_template(element, &source_template);
}

void gst_hm_preview_gpu_surface_init(GstHmPreviewGpuSurface* self) {
  self->state = new (std::nothrow) State;
  self->sink = gst_pad_new_from_static_template(&sink_template, "sink");
  self->src = gst_pad_new_from_static_template(&source_template, "src");
  gst_pad_set_chain_function(self->sink, chain);
  gst_pad_set_event_function(self->sink, event);
  gst_pad_set_query_function(self->sink, query);
  gst_pad_set_query_function(self->src, query);
  gst_element_add_pad(GST_ELEMENT(self), self->sink);
  gst_element_add_pad(GST_ELEMENT(self), self->src);
}
} // namespace

namespace hm::preview {
GstElement* gpu_surface_create(const char* name, const GpuSurfaceConfig& config, SurfacePainter painter) {
  if (!config.width || !config.height || config.width > 8192 || config.height > 8192 || !config.rate_numerator ||
      !config.rate_denominator || config.rate_denominator > 1000000 ||
      double(config.rate_numerator) / config.rate_denominator > 120 || !config.pool_frames || config.pool_frames > 64)
    return nullptr;
  auto* self = reinterpret_cast<GstHmPreviewGpuSurface*>(
      g_object_new(gst_hm_preview_gpu_surface_get_type(), "name", name, nullptr));
  if (!self || !self->state) {
    if (self)
      gst_object_unref(self);
    return nullptr;
  }
  self->state->config = config;
  self->state->painter = std::move(painter);
  return GST_ELEMENT(self);
}
} // namespace hm::preview
