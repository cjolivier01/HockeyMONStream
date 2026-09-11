#include "hstream/src/apps/apps-common/EncoderDimensions.h"

#include <dlfcn.h>

#include <algorithm>
#include <cmath>

#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
#include <linux/videodev2.h>
#else
#include <cuda.h>
#include <ffnvcodec/nvEncodeAPI.h>
#endif

namespace hm {
namespace {

struct Library {
  void* handle;
  explicit Library(const char* name) : handle(dlopen(name, RTLD_NOW | RTLD_LOCAL)) {}
  ~Library() {
    if (handle)
      dlclose(handle);
  }
  template <typename T>
  T symbol(const char* name) const {
    return handle ? reinterpret_cast<T>(dlsym(handle, name)) : nullptr;
  }
};

struct DimensionProbe {
  GstElement* caps_filter;
  EncoderDimensionLimits limits;
};

GstPadProbeReturn update_dimensions(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto* state = static_cast<DimensionProbe*>(data);
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (GST_EVENT_TYPE(event) != GST_EVENT_CAPS)
    return GST_PAD_PROBE_OK;
  GstCaps* input_caps = nullptr;
  gst_event_parse_caps(event, &input_caps);
  const GstStructure* input = gst_caps_get_structure(input_caps, 0);
  gint width = 0;
  gint height = 0;
  if (!gst_structure_get_int(input, "width", &width) || !gst_structure_get_int(input, "height", &height) ||
      width <= 0 || height <= 0)
    return GST_PAD_PROBE_OK;
  auto fitted = fit_encoder_dimensions(width, height, state->limits);
  if (!fitted) {
    GST_ELEMENT_ERROR(
        state->caps_filter,
        STREAM,
        FORMAT,
        ("Stitched canvas cannot fit the encoder's supported dimensions"),
        ("Input %dx%d; encoder range %ux%u to %ux%u",
         width,
         height,
         state->limits.min_width,
         state->limits.min_height,
         state->limits.max_width,
         state->limits.max_height));
    return GST_PAD_PROBE_DROP;
  }
  GstCaps* caps = nullptr;
  g_object_get(state->caps_filter, "caps", &caps, nullptr);
  caps = gst_caps_make_writable(caps);
  gst_caps_set_simple(
      caps,
      "width",
      G_TYPE_INT,
      static_cast<gint>(fitted->first),
      "height",
      G_TYPE_INT,
      static_cast<gint>(fitted->second),
      nullptr);
  g_object_set(state->caps_filter, "caps", caps, nullptr);
  gst_caps_unref(caps);
  if (fitted->first != static_cast<guint>(width) || fitted->second != static_cast<guint>(height)) {
    g_message(
        "Stitched archive encoder: scaling %dx%d to %ux%u (encoder maximum %ux%u)",
        width,
        height,
        fitted->first,
        fitted->second,
        state->limits.max_width,
        state->limits.max_height);
  }
  return GST_PAD_PROBE_OK;
}

} // namespace

std::optional<std::pair<guint, guint>> fit_encoder_dimensions(
    guint width,
    guint height,
    const EncoderDimensionLimits& limits) {
  if (width < 2 || height < 2 || limits.max_width < 2 || limits.max_height < 2)
    return std::nullopt;
  const double scale = std::min({1.0, double(limits.max_width) / width, double(limits.max_height) / height});
  auto dimensions = [&](double factor) {
    return std::make_pair(static_cast<guint>(width * factor) & ~1U, static_cast<guint>(height * factor) & ~1U);
  };
  auto fitted = dimensions(scale);
  auto within_budget = [&](const auto& size) {
    return !limits.max_macroblocks ||
        guint64((size.first + 15) / 16) * ((size.second + 15) / 16) <= limits.max_macroblocks;
  };
  if (!within_budget(fitted)) {
    double low = 0;
    double high = scale;
    for (int i = 0; i < 48; ++i) {
      const double mid = (low + high) / 2;
      if (within_budget(dimensions(mid)))
        low = mid;
      else
        high = mid;
    }
    fitted = dimensions(low);
  }
  if (fitted.first < std::max(2U, limits.min_width) || fitted.second < std::max(2U, limits.min_height))
    return std::nullopt;
  return fitted;
}

std::optional<EncoderDimensionLimits> query_encoder_dimensions(GstElement* encoder, bool hevc, guint gpu_id) {
  EncoderDimensionLimits limits;
#if defined(__aarch64__) && !defined(AARCH64_IS_SBSA)
  Library v4l2("libv4l2.so.0");
  auto ioctl_fn = v4l2.symbol<int (*)(int, unsigned long, ...)>("v4l2_ioctl");
  if (!ioctl_fn || !g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), "device-fd"))
    return std::nullopt;
  // The Jetson driver exposes codec-specific ranges through ENUM_FRAMESIZES.
  // READY opens the device; restore NULL before the containing bin takes over its state.
  if (gst_element_set_state(encoder, GST_STATE_READY) == GST_STATE_CHANGE_FAILURE) {
    gst_element_set_state(encoder, GST_STATE_NULL);
    return std::nullopt;
  }
  gint fd = -1;
  g_object_get(encoder, "device-fd", &fd, nullptr);
  v4l2_frmsizeenum sizes{};
  sizes.pixel_format = hevc ? v4l2_fourcc('H', '2', '6', '5') : V4L2_PIX_FMT_H264;
  const bool ok = fd >= 0 && ioctl_fn(fd, VIDIOC_ENUM_FRAMESIZES, &sizes) == 0 &&
      (sizes.type == V4L2_FRMSIZE_TYPE_STEPWISE || sizes.type == V4L2_FRMSIZE_TYPE_CONTINUOUS);
  gst_element_set_state(encoder, GST_STATE_NULL);
  if (!ok)
    return std::nullopt;
  limits = {sizes.stepwise.min_width, sizes.stepwise.min_height, sizes.stepwise.max_width, sizes.stepwise.max_height};
#else
  Library cuda("libcuda.so.1");
  Library nvenc("libnvidia-encode.so.1");
  auto init = cuda.symbol<decltype(&cuInit)>("cuInit");
  auto get_device = cuda.symbol<decltype(&cuDeviceGet)>("cuDeviceGet");
  auto retain = cuda.symbol<decltype(&cuDevicePrimaryCtxRetain)>("cuDevicePrimaryCtxRetain");
  auto release = cuda.symbol<decltype(&cuDevicePrimaryCtxRelease)>("cuDevicePrimaryCtxRelease_v2");
  auto create = nvenc.symbol<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>("NvEncodeAPICreateInstance");
  if (!init || !get_device || !retain || !release || !create || init(0) != CUDA_SUCCESS)
    return std::nullopt;
  CUdevice device;
  CUcontext context;
  if (get_device(&device, gpu_id) != CUDA_SUCCESS || retain(&context, device) != CUDA_SUCCESS)
    return std::nullopt;
  NV_ENCODE_API_FUNCTION_LIST api{};
  api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
  NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params{};
  params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
  params.apiVersion = NVENCAPI_VERSION;
  params.device = context;
  params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
  void* session = nullptr;
  bool ok = create(&api) == NV_ENC_SUCCESS && api.nvEncOpenEncodeSessionEx(&params, &session) == NV_ENC_SUCCESS;
  auto query = [&](NV_ENC_CAPS cap, guint& value) {
    NV_ENC_CAPS_PARAM params{};
    params.version = NV_ENC_CAPS_PARAM_VER;
    params.capsToQuery = cap;
    int result = 0;
    if (api.nvEncGetEncodeCaps(session, hevc ? NV_ENC_CODEC_HEVC_GUID : NV_ENC_CODEC_H264_GUID, &params, &result) !=
            NV_ENC_SUCCESS ||
        result <= 0)
      return false;
    value = result;
    return true;
  };
  ok = ok && query(NV_ENC_CAPS_WIDTH_MIN, limits.min_width) && query(NV_ENC_CAPS_HEIGHT_MIN, limits.min_height) &&
      query(NV_ENC_CAPS_WIDTH_MAX, limits.max_width) && query(NV_ENC_CAPS_HEIGHT_MAX, limits.max_height) &&
      query(NV_ENC_CAPS_MB_NUM_MAX, limits.max_macroblocks);
  if (session)
    api.nvEncDestroyEncoder(session);
  release(device);
  if (!ok)
    return std::nullopt;
#endif
  if (limits.max_width < limits.min_width || limits.max_height < limits.min_height || limits.max_width > G_MAXINT ||
      limits.max_height > G_MAXINT)
    return std::nullopt;
  GST_INFO_OBJECT(
      encoder,
      "Encoder dimension limits: %ux%u to %ux%u, max macroblocks %u",
      limits.min_width,
      limits.min_height,
      limits.max_width,
      limits.max_height,
      limits.max_macroblocks);
  return limits;
}

bool install_encoder_dimension_limit(
    GstElement* converter,
    GstElement* caps_filter,
    const EncoderDimensionLimits& limits) {
  GstPad* sink = gst_element_get_static_pad(converter, "sink");
  if (!sink)
    return false;
  auto* state = new DimensionProbe{GST_ELEMENT(gst_object_ref(caps_filter)), limits};
  const auto probe =
      gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, update_dimensions, state, [](gpointer data) {
        auto* state = static_cast<DimensionProbe*>(data);
        gst_object_unref(state->caps_filter);
        delete state;
      });
  gst_object_unref(sink);
  return probe != 0;
}

} // namespace hm
