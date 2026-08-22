#include "gstvideoprep.h"

#include <cuda_runtime.h>
#include <glib-2.0/glib.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <gstreamer-1.0/gst/gstbuffer.h>
#include <gstreamer-1.0/gst/gstinfo.h>
#include <gstreamer-1.0/gst/gstpad.h>
#include <npp.h>
#include <cmath>
#include <iostream>
#include <mutex>
#include <string_view>
// #include "deepstream/sources/includes/nvbufsurface.h"
#include "gst-nvcommon.h"
#include "gstnvdsmeta.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/gst_utils.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/hmcustomlib_interface.hpp"
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"
#include "nvds_dewarper_meta.h"
#include "nvdsmeta.h"
#include "videoprep_plugins.h"
#include "videoprep_property_parser.h"

#include <assert.h>
#include <cuda.h>
#include <string.h>
#include <unistd.h>

#include "absl/strings/str_split.h"

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

#define DEFAULT_NUM_VIDEO_PREPPED_SURFACES (4)
#define DEFAULT_DEWARP_DUMP_FRAMES 0

#define DEFAULT_DEWARP_OUTPUT_WIDTH 0
#define DEFAULT_DEWARP_OUTPUT_HEIGHT 0

// #define DEFAULT_DEWARP_OUTPUT_WIDTH 256
// #define DEFAULT_DEWARP_OUTPUT_HEIGHT 128

#define USE_CUDA_STREAM

#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"

#define DEFAULT_GPU_ID 0
#define DEFAULT_SOURCE_ID 0
#define DEFAULT_NUM_OUTPUT_BUFFERS 4
#define MAX_BUFFERS 4

// #define MEASURE_TIME
#ifdef MEASURE_TIME
#include <stdio.h>
#include <sys/time.h>

#define START_PROFILE         \
  {                           \
    struct timeval t1, t2;    \
    double elapsedTime = 0;   \
    double totalReadTime = 0; \
    gettimeofday(&t1, NULL);

#define STOP_PROFILE(X)                                     \
  gettimeofday(&t2, NULL);                                  \
  elapsedTime = (t2.tv_sec - t1.tv_sec) * 1000.0;           \
  elapsedTime += (t2.tv_usec - t1.tv_usec) / 1000.0;        \
  totalReadTime += elapsedTime;                             \
  printf(                                                   \
      "(%s)  %p : #%d %s ElaspedTime=%f TotalTime=%f ms\n", \
      GST_ELEMENT_NAME(videoprep),                          \
      videoprep,                                            \
      videoprep->frame_num,                                 \
      X,                                                    \
      elapsedTime,                                          \
      totalReadTime);                                       \
  }

#else
#define START_PROFILE
#define STOP_PROFILE(X)
#endif

namespace hm {
namespace videoprep {

static guint get_output_batch_size(GstVideoPrep* videoprep, GstCaps* input_caps) {
  guint input_batch_size = 0;
  if (input_caps && !gst_caps_is_any(input_caps) && !gst_caps_is_empty(input_caps) &&
      gst_caps_get_size(input_caps) > 0) {
    GstStructure* input_structure = gst_caps_get_structure(input_caps, 0);
    if (!gst_structure_get_uint(input_structure, "batch-size", &input_batch_size)) {
      gint signed_input_batch_size = 0;
      if (gst_structure_get_int(input_structure, "batch-size", &signed_input_batch_size) &&
          signed_input_batch_size > 0) {
        input_batch_size = static_cast<guint>(signed_input_batch_size);
      }
    }
  }
  if (videoprep->priv) {
    const guint output_batch_size = videoprep->priv->GetOutputBatchSize(input_batch_size, videoprep->num_batch_buffers);
    if (output_batch_size > 0) {
      return output_batch_size;
    }
  }
  return videoprep->num_batch_buffers ? videoprep->num_batch_buffers : 1;
}

// Helper macros for alignment
#define NVBUF_ALIGN_VAL (256)
#define NVBUF_ALIGN_PITCH(pitch, align_val) ((pitch % align_val == 0) ? pitch : ((pitch / align_val + 1) * align_val))
#define NVBUF_PLATFORM_ALIGNED_PITCH(pitch) NVBUF_ALIGN_PITCH(pitch, NVBUF_ALIGN_VAL)

static gchar VIDEOPREP_LIB_VERSION[128];

enum {
  /* FILL ME */
  MEM_FEATURE_NVMM,
  MEM_FEATURE_RAW
};
/* Filter signals and args */
enum {
  /* FILL ME */
  LAST_SIGNAL
};

enum {
  PROP_0,
  PROP_GPU_DEVICE_ID,
  PROP_SOURCE_ID,
  PROP_NUM_OUTPUT_BUFFERS,
  PROP_CONFIG_FILE,
  PROP_PLUGIN_TYPE,
  PROP_OUTPUT_WIDTH,
  PROP_OUTPUT_HEIGHT,
  PROP_NUM_BATCH_BUFFERS,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_INTERPOLATION_METHOD,
  PROP_PLUGIN_PRIVATE_CONFIG,
  PROP_POST_STITCH_ROTATE_DEGREES,
  PROP_FIXED_EDGE_ROTATION_ANGLE,
  PROP_FIXED_EDGE_ROTATION_ANGLE_LEFT,
  PROP_FIXED_EDGE_ROTATION_ANGLE_RIGHT,
  PROP_DYNAMIC_ACCELERATION_SCALING,
  PROP_PREVIEW_OVERLAY_FLAGS,
  PROP_HIGH_BIT_DEPTH,
  PROP_SHADOW_LIFT,
  PROP_SHADOW_LIFT_BLACK_POINT,
  PROP_EXPOSURE,
  PROP_RUNTIME_TUNING_CONFIG_FILE,
  PROP_CANCEL_PENDING_WORK,
  PROP_LAST_PROPERTY_SET_OK,
  PROP_SILENT,
};

#define GST_ERROR_ON_BUS(msg, ...)                                                                              \
  do {                                                                                                          \
    if (videoprep) {                                                                                            \
      GST_ERROR_OBJECT(videoprep, __VA_ARGS__);                                                                 \
      GError* err = g_error_new(g_quark_from_static_string(GST_ELEMENT_NAME(videoprep)), -1, __VA_ARGS__);      \
      gst_element_post_message(GST_ELEMENT(videoprep), gst_message_new_error(GST_OBJECT(videoprep), err, msg)); \
    }                                                                                                           \
  } while (0)

static void gst_videoprep_finalize(GObject* object);
static bool gst_videoprep_apply_typed_properties(GstVideoPrep* videoprep);

// static const gchar* print_pretty_time(gchar* ts_str, gsize ts_str_len, GstClockTime ts) {
//   if (ts == GST_CLOCK_TIME_NONE)
//     return "none";

//   g_snprintf(ts_str, ts_str_len, "%" GST_TIME_FORMAT, GST_TIME_ARGS(ts));
//   return ts_str;
// }

inline bool NPP_CHECK_(gint e, gint iLine, const gchar* szFile) {
  if (e != NPP_SUCCESS) {
    std::cout << "Dewarper: NPP API error " << e << " at line " << iLine << " in file " << szFile << std::endl;
    exit(-1);
    return false;
  }
  return true;
}

#define npp_ck(call) NPP_CHECK_(call, __LINE__, __FILE__)
/* the capabilities of the inputs and outputs.
 *
 * describe the real formats here.
 */
/* Input capabilities. */
static GstStaticPadTemplate sink_factory = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(
        GST_CAPS_FEATURE_MEMORY_NVMM,
        "{ "
        "RGBA, RGB10A2_LE }")));

/* Output capabilities. */
static GstStaticPadTemplate src_factory = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(
        GST_CAPS_FEATURE_MEMORY_NVMM,
        "{ "
        "RGBA }")));

#define gst_videoprep_parent_class parent_class
G_DEFINE_TYPE(GstVideoPrep, gst_videoprep, GST_TYPE_BASE_TRANSFORM);

static void gst_videoprep_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);

static void gst_videoprep_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

static gpointer videoprep_meta_copy_func(gpointer data, gpointer user_data) {
  NvDewarperSurfaceMeta* src_surface_meta = (NvDewarperSurfaceMeta*)data;
  assert(src_surface_meta);
  NvDewarperSurfaceMeta* dst_surface_meta = (NvDewarperSurfaceMeta*)g_malloc0(sizeof(NvDewarperSurfaceMeta));
  memcpy(dst_surface_meta, src_surface_meta, sizeof(NvDewarperSurfaceMeta));
  return (gpointer)dst_surface_meta;
}

static void videoprep_meta_release_func(gpointer data, gpointer user_data) {
  NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)data;
  if (surface_meta) {
    g_free(surface_meta);
    surface_meta = NULL;
  }
}

static gpointer videoprep_gst_to_nvds_meta_transform_func(gpointer data, gpointer user_data) {
  NvDsUserMeta* user_meta = (NvDsUserMeta*)data;
  NvDewarperSurfaceMeta* src_surface_meta = (NvDewarperSurfaceMeta*)user_meta->user_meta_data;
  assert(src_surface_meta);
  NvDewarperSurfaceMeta* dst_surface_meta = (NvDewarperSurfaceMeta*)videoprep_meta_copy_func(src_surface_meta, NULL);
  return (gpointer)dst_surface_meta;
}

static void videoprep_gst_nvds_meta_release_func(gpointer data, gpointer user_data) {
  NvDsUserMeta* user_meta = (NvDsUserMeta*)data;
  NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)user_meta->user_meta_data;
  assert(surface_meta);
  videoprep_meta_release_func(surface_meta, NULL);
}

static gboolean gst_videoprep_accept_caps(GstBaseTransform* btrans, GstPadDirection direction, GstCaps* caps) {
  gboolean ret = TRUE;
  GstVideoPrep* videoprep = NULL;
  GstCaps* allowed = NULL;

  videoprep = GST_VIDEOPREP(btrans);

  GST_DEBUG_OBJECT(videoprep, "accept caps %" GST_PTR_FORMAT, caps);

  GstVideoInfo vid_info = {
      0,
  };
  if (!gst_video_info_from_caps(&vid_info, caps)) {
    GST_ERROR("invalid input caps");
    return FALSE;
  }

  /* get all the formats we can handle on this pad */
  if (direction == GST_PAD_SINK) {
    allowed = videoprep->sinkcaps;
    videoprep->input_width = vid_info.width;
    videoprep->input_height = vid_info.height;
  } else {
    allowed = videoprep->srccaps;
    if (videoprep->output_width && videoprep->output_width != (guint)vid_info.width) {
      goto no_transform_possible;
    }
    if (videoprep->output_height && videoprep->output_height != (guint)vid_info.height) {
      goto no_transform_possible;
    }
  }

  if (!allowed) {
    GST_DEBUG_OBJECT(videoprep, "failed to get allowed caps");
    goto no_transform_possible;
  }

  GST_DEBUG_OBJECT(videoprep, "allowed caps %" GST_PTR_FORMAT, allowed);

  /* intersect with the requested format */
  ret = gst_caps_is_subset(caps, allowed);
  if (!ret) {
    goto no_transform_possible;
  }

done:
  return ret;

/* ERRORS */
no_transform_possible: {
  GST_DEBUG_OBJECT(videoprep, "could not transform %" GST_PTR_FORMAT " in anything we support", caps);
  ret = FALSE;
  goto done;
}
}

std::mutex mini_mu_;
std::unordered_map<GstMiniObject*, GstMiniObjectDisposeFunction> mini_disposes_;

static gboolean gst_videoprep_buffer_destruct_notify(GstMiniObject* mini) {
  std::unique_lock lk(mini_mu_);
  g_print("GstBuffer %p is being released\n", mini);
  assert(mini_disposes_.count(mini));
  auto fun = mini_disposes_.at(mini);
  if (mini->dispose == &gst_videoprep_buffer_destruct_notify) {
    mini->dispose = fun;
  }
  mini_disposes_.erase(mini);
  if (fun) {
    return (*fun)(mini);
  }
  return TRUE;
}

void gst_videoprep_hook_buffer_release(GstBuffer* buffer) {
  GstMiniObject* mini = GST_MINI_OBJECT(buffer);
  assert((size_t)mini == (size_t)buffer);
  std::unique_lock lk(mini_mu_);
  assert(!mini_disposes_.count(mini));
  mini_disposes_.emplace(mini, mini->dispose);
  mini->dispose = &gst_videoprep_buffer_destruct_notify;
  g_print("GstBuffer %p is hooked\n", buffer);
}

static GstCaps* gst_videoprep_fixate_caps(
    GstBaseTransform* trans,
    GstPadDirection direction,
    GstCaps* caps,
    GstCaps* othercaps) {
  GstStructure *ins, *outs;
  const GValue *from_par, *to_par;
  const gchar *from_fmt = NULL, *to_fmt = NULL;
  GstVideoPrep* videoprep = GST_VIDEOPREP(trans);

  guint out_width, out_height;
  const bool fixating_output = direction == GST_PAD_SINK;
  const bool runtime_output_size = fixating_output && videoprep->priv && videoprep->priv->UsesRuntimeOutputSize();
  const RuntimeOutputSize negotiated_runtime_size =
      fixating_output && videoprep->priv ? videoprep->priv->RuntimeOutputSizeForNegotiation() : RuntimeOutputSize{};

  othercaps = gst_caps_truncate(othercaps);
  othercaps = gst_caps_make_writable(othercaps);

  GST_DEBUG_OBJECT(
      trans,
      "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %" GST_PTR_FORMAT " current direction is %s",
      othercaps,
      caps,
      (direction == GST_PAD_SINK) ? "sink" : "src");

  ins = gst_caps_get_structure(caps, 0);
  outs = gst_caps_get_structure(othercaps, 0);
  if (fixating_output) {
    gst_structure_set(outs, "batch-size", G_TYPE_UINT, get_output_batch_size(videoprep, caps), NULL);

    if (!runtime_output_size && !negotiated_runtime_size.valid() &&
        videoprep->custom_create_params.output_width_height[0]) {
      videoprep->output_width = videoprep->custom_create_params.output_width_height[0];
    }
    if (!runtime_output_size && !negotiated_runtime_size.valid() &&
        videoprep->custom_create_params.output_width_height[1]) {
      videoprep->output_height = videoprep->custom_create_params.output_width_height[1];
    }
    if (!runtime_output_size && !negotiated_runtime_size.valid() && !videoprep->output_width &&
        !videoprep->output_height) {
      videoprep->output_width = videoprep->input_width;
      videoprep->output_height = videoprep->input_height;
      assert(videoprep->output_width && videoprep->output_height);
    }

    out_width = negotiated_runtime_size.valid() ? negotiated_runtime_size.width : videoprep->output_width;
    out_height = negotiated_runtime_size.valid() ? negotiated_runtime_size.height : videoprep->output_height;
    if (runtime_output_size && !negotiated_runtime_size.valid()) {
      gint input_width = 0;
      gint input_height = 0;
      gst_structure_get_int(ins, "width", &input_width);
      gst_structure_get_int(ins, "height", &input_height);
      out_width = input_width;
      out_height = input_height;
    }

    gst_structure_remove_fields(outs, "width", "height", NULL);
    gst_structure_set(outs, "width", G_TYPE_INT, out_width, "height", G_TYPE_INT, out_height, NULL);
  } else if (videoprep->input_width && videoprep->input_height) {
    // Reverse negotiation fixes the sink/input caps. A discovered stitched
    // canvas is an output property and must never be imposed on the cameras.
    // Retain the dimensions accepted on the sink side when they are known.
    gst_structure_fixate_field_nearest_int(outs, "width", videoprep->input_width);
    gst_structure_fixate_field_nearest_int(outs, "height", videoprep->input_height);
  }

  from_fmt = gst_structure_get_string(ins, "format");
  to_fmt = gst_structure_get_string(outs, "format");

  if (!to_fmt) {
    /* Output format not fixed */
    if (!gst_structure_fixate_field_string(outs, "format", from_fmt)) {
      return NULL;
    }
  }

  from_par = gst_structure_get_value(ins, "pixel-aspect-ratio");
  to_par = gst_structure_get_value(outs, "pixel-aspect-ratio");

  /* we have both PAR but they might not be fixated */
  if (from_par && to_par) {
    gint from_w = 0, from_h = 0, from_par_n = 0, from_par_d = 0, to_par_n = 0, to_par_d = 0;
    gint count = 0, w = 0, h = 0;
    guint num = 0, den = 0;

    /* from_par should be fixed */
    g_return_val_if_fail(gst_value_is_fixed(from_par), othercaps);

    from_par_n = gst_value_get_fraction_numerator(from_par);
    from_par_d = gst_value_get_fraction_denominator(from_par);

    /* fixate the out PAR */
    if (!gst_value_is_fixed(to_par)) {
      GST_DEBUG_OBJECT(trans, "fixating to_par to %dx%d", from_par_n, from_par_d);
      gst_structure_fixate_field_nearest_fraction(outs, "pixel-aspect-ratio", from_par_n, from_par_d);
    }

    to_par_n = gst_value_get_fraction_numerator(to_par);
    to_par_d = gst_value_get_fraction_denominator(to_par);

    /* if both width and height are already fixed, we can't do anything
     * about it anymore */
    if (gst_structure_get_int(outs, "width", &w))
      ++count;
    if (gst_structure_get_int(outs, "height", &h))
      ++count;
    if (count == 2) {
      GST_DEBUG_OBJECT(trans, "dimensions already set to %dx%d, not fixating", w, h);
      g_print("%s: line=%d ---- %s\n", GST_ELEMENT_NAME(trans), __LINE__, gst_caps_to_string(othercaps));
      return othercaps;
    }

    gst_structure_get_int(ins, "width", &from_w);
    gst_structure_get_int(ins, "height", &from_h);

    if (!gst_video_calculate_display_ratio(&num, &den, from_w, from_h, from_par_n, from_par_d, to_par_n, to_par_d)) {
      GST_ELEMENT_ERROR(
          trans, CORE, NEGOTIATION, (NULL), ("Error calculating the output scaled size - integer overflow"));
      g_print("%s: line=%d ---- %s\n", GST_ELEMENT_NAME(trans), __LINE__, gst_caps_to_string(othercaps));
      return othercaps;
    }

    GST_DEBUG_OBJECT(
        trans,
        "scaling input with %dx%d and PAR %d/%d to output PAR %d/%d",
        from_w,
        from_h,
        from_par_n,
        from_par_d,
        to_par_n,
        to_par_d);
    GST_DEBUG_OBJECT(trans, "resulting output should respect ratio of %d/%d", num, den);

    /* now find a width x height that respects this display ratio.
     * prefer those that have one of w/h the same as the incoming video
     * using wd / hd = num / den */

    /* if one of the output width or height is fixed, we work from there */
    if (h) {
      GST_DEBUG_OBJECT(trans, "height is fixed,scaling width");
      w = (guint)gst_util_uint64_scale_int(h, num, den);
    } else if (w) {
      GST_DEBUG_OBJECT(trans, "width is fixed, scaling height");
      h = (guint)gst_util_uint64_scale_int(w, den, num);
    } else {
      /* none of width or height is fixed, figure out both of them based only on
       * the input width and height */
      /* check hd / den is an integer scale factor, and scale wd with the PAR */
      if (from_h % den == 0) {
        GST_DEBUG_OBJECT(trans, "keeping video height");
        h = from_h;
        w = (guint)gst_util_uint64_scale_int(h, num, den);
      } else if (from_w % num == 0) {
        GST_DEBUG_OBJECT(trans, "keeping video width");
        w = from_w;
        h = (guint)gst_util_uint64_scale_int(w, den, num);
      } else {
        GST_DEBUG_OBJECT(trans, "approximating but keeping video height");
        h = from_h;
        w = (guint)gst_util_uint64_scale_int(h, num, den);
      }
    }
    GST_DEBUG_OBJECT(trans, "scaling to %dx%d", w, h);

    /* now fixate */
    gst_structure_fixate_field_nearest_int(outs, "width", w);
    gst_structure_fixate_field_nearest_int(outs, "height", h);
  } else {
    gint width, height;

    if (gst_structure_get_int(ins, "width", &width)) {
      if (gst_structure_has_field(outs, "width")) {
        gst_structure_fixate_field_nearest_int(outs, "width", width);
      }
    }
    if (gst_structure_get_int(ins, "height", &height)) {
      if (gst_structure_has_field(outs, "height")) {
        gst_structure_fixate_field_nearest_int(outs, "height", height);
      }
    }
  }
  if (fixating_output) {
    GstCaps* peer_caps = gst_pad_peer_query_caps(GST_BASE_TRANSFORM_SRC_PAD(trans), NULL);
    GstStructure* peer_structure;
    const gchar* out_mem_type_string = NULL;
    int n = gst_caps_get_size(peer_caps);
    bool peer_has_gpu_id = false;
    if (n > 0) {
      peer_caps = gst_caps_truncate(peer_caps);
      GST_DEBUG_OBJECT(trans, "peer caps %" GST_PTR_FORMAT, peer_caps);
      peer_structure = gst_caps_get_structure(peer_caps, 0);
      out_mem_type_string = gst_structure_get_string(peer_structure, "nvbuf-memory-type");
      peer_has_gpu_id = gst_structure_has_field(peer_structure, "gpu-id");
    }

    if (!out_mem_type_string) {
      int mem_type = videoprep->cuda_mem_type;
      if (mem_type == NVBUF_MEM_DEFAULT)
        GET_DEFAULT_MEM_TYPE(mem_type);
      g_assert(mem_type != NVBUF_MEM_DEFAULT);
      gst_structure_set(outs, "nvbuf-memory-type", G_TYPE_STRING, gst_nvbuf_memory_get_name(mem_type), NULL);
    } else {
      videoprep->cuda_mem_type = (NvBufSurfaceMemType)gst_nvbuf_memory_get_value(out_mem_type_string);
      if (videoprep->cuda_mem_type <= NVBUF_MEM_DEFAULT)
        GST_ERROR_OBJECT(trans, "Incorrect nvbuf-memory-type set on src pad!!");
      GST_WARNING_OBJECT(
          trans,
          "nvbuf-memory-type property is set based on SRC caps. Property config setting (if any) is overridden!!");
      gst_structure_set(outs, "nvbuf-memory-type", G_TYPE_STRING, out_mem_type_string, NULL);
    }
    if (peer_has_gpu_id) {
      gst_structure_get_int(peer_structure, "gpu-id", (int*)&videoprep->gpu_id);
      GST_WARNING_OBJECT(
          trans, "gpu-id property is set based on SRC caps. Property config setting (if any) is overridden!!");
    }
    gst_structure_set(outs, "gpu-id", G_TYPE_INT, videoprep->gpu_id, NULL);
    gst_caps_unref(peer_caps);
  }

  GST_DEBUG_OBJECT(trans, "fixated othercaps to %" GST_PTR_FORMAT, othercaps);

  // g_print ("%s: line=%d ---- %s\n", GST_ELEMENT_NAME(trans), __LINE__, gst_caps_to_string(othercaps));
  return othercaps;
}

static GstCaps* gst_videoprep_transform_caps(
    GstBaseTransform* btrans,
    GstPadDirection direction,
    GstCaps* caps,
    GstCaps* filter) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(btrans);
  GstCapsFeatures* feature = NULL;
  GstCaps* new_caps = NULL;
  GstCaps* temp_caps = NULL;

  if (direction == GST_PAD_SINK) {
    const guint output_batch_size = get_output_batch_size(videoprep, caps);
    const RuntimeOutputSize runtime_size =
        videoprep->priv ? videoprep->priv->RuntimeOutputSizeForNegotiation() : RuntimeOutputSize{};
    if (runtime_size.valid()) {
      new_caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "RGBA",
          "width",
          G_TYPE_INT,
          static_cast<gint>(runtime_size.width),
          "height",
          G_TYPE_INT,
          static_cast<gint>(runtime_size.height),
          "batch-size",
          G_TYPE_UINT,
          runtime_size.batch_size > 0 ? runtime_size.batch_size : output_batch_size,
          NULL);
    } else if (!videoprep->output_width && !videoprep->output_height) {
      new_caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "RGBA",
          "width",
          GST_TYPE_INT_RANGE,
          1,
          G_MAXINT,
          "height",
          GST_TYPE_INT_RANGE,
          1,
          G_MAXINT,
          "batch-size",
          G_TYPE_UINT,
          output_batch_size,
          NULL);
    } else {
      assert(videoprep->output_width && videoprep->output_height);
      new_caps = gst_caps_new_simple(
          "video/x-raw",
          "format",
          G_TYPE_STRING,
          "RGBA",
          "width",
          G_TYPE_INT,
          videoprep->output_width,
          "height",
          G_TYPE_INT,
          videoprep->output_height,
          "batch-size",
          G_TYPE_UINT,
          output_batch_size,
          NULL);
    }
    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(new_caps, 0, feature);
  }
  if (direction == GST_PAD_SRC) {
    new_caps = gst_caps_new_simple(
        "video/x-raw",
        "format",
        G_TYPE_STRING,
        videoprep->high_bit_depth ? "RGB10A2_LE" : "RGBA",
        "width",
        GST_TYPE_INT_RANGE,
        1,
        G_MAXINT,
        "height",
        GST_TYPE_INT_RANGE,
        1,
        G_MAXINT,
        NULL);
    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(new_caps, 0, feature);
  }

  if (gst_caps_is_fixed(caps)) {
    GstStructure* fs = gst_caps_get_structure(caps, 0);
    const GValue* fps_value;
    guint i, n = gst_caps_get_size(new_caps);

    fps_value = gst_structure_get_value(fs, "framerate");

    // We cannot change framerate
    for (i = 0; i < n; i++) {
      fs = gst_caps_get_structure(new_caps, i);
      gst_structure_set_value(fs, "framerate", fps_value);
    }
  }
  if (filter) {
    temp_caps = gst_caps_intersect(new_caps, filter);
    gst_caps_unref(new_caps);
    new_caps = temp_caps;
  }
  return new_caps;
}

static void gst_videoprep_state_changed(GstElement* element, GstState oldstate, GstState newstate, GstState pending) {
  GstVideoPrepClass* klass = GST_VIDEOPREP_CLASS(element);
  // GstVideoPrep* videoprep = GST_VIDEOPREP(element);
  // if (oldstate == GstState::GST_STATE_NULL && newstate == GstState::GST_STATE_READY) {
  //   std::cout << "state change here" << std::endl;
  // }
  if (klass->parent_state_changed_fn) {
    klass->parent_state_changed_fn(element, oldstate, newstate, pending);
  }
}

static void gst_videoprep_clear_priv(GstVideoPrep* videoprep) {
  if (!videoprep || !videoprep->priv) {
    return;
  }
  delete videoprep->priv;
  videoprep->priv = nullptr;
}

static GstStateChangeReturn gst_videoprep_change_state(GstElement* element, GstStateChange transition) {
  if (transition == GST_STATE_CHANGE_NULL_TO_READY) {
    GstVideoPrep* videoprep = GST_VIDEOPREP(element);
    videoprep->custom_create_params = DSCustom_CreateParams();
    videoprep->custom_create_params.m_element = (GstBaseTransform*)element;

    assert(!videoprep->priv);
    GObject* object = G_OBJECT(element);
    assert(object);
    assert(videoprep->plugin_type);
    if (!videoprep->plugin_type || !*videoprep->plugin_type) {
      std::cerr << "Plugin type not set (videoprep)" << std::endl;
      return GST_STATE_CHANGE_FAILURE;
    }
    videoprep->priv = dynamic_cast<VideoPrepPriv*>(videoprep->priv_factory->CreateCustomAlgoCtx(
        videoprep->plugin_type, object, videoprep->gpu_id, videoprep->num_batch_buffers));
    if (!videoprep->priv) {
      GST_ERROR("Unable to create plugin type %s", videoprep->plugin_type);
      return GST_STATE_CHANGE_FAILURE;
    }
    if (videoprep->plugin_private_config) {
      if (!videoprep->priv->SetPrivateConfig(videoprep->plugin_private_config)) {
        GST_ERROR("Invalid plugin private config for %s", videoprep->plugin_type);
        gst_videoprep_clear_priv(videoprep);
        return GST_STATE_CHANGE_FAILURE;
      }
    }
    if (!gst_videoprep_apply_typed_properties(videoprep)) {
      GST_ERROR("Invalid typed property for %s", videoprep->plugin_type);
      gst_videoprep_clear_priv(videoprep);
      return GST_STATE_CHANGE_FAILURE;
    }
    videoprep->custom_create_params.config_file = videoprep->config_file;
    absl::Status status = videoprep->priv->PreCapsInit(&videoprep->custom_create_params);
    if (!status.ok()) {
      std::cerr << status << std::endl;
      GST_ERROR("Error on bus: SetInitParams Error");
      gst_videoprep_clear_priv(videoprep);
      return GST_STATE_CHANGE_FAILURE;
    }
  } else if (transition == GST_STATE_CHANGE_PAUSED_TO_READY) {
    // std::cout << "stopping..." << std::endl;
  }
  GstVideoPrepClass* klass = GST_VIDEOPREP_CLASS(element);
  assert(GST_IS_VIDEOPREP_CLASS(klass));
  assert(klass->parent_change_state_fn);
  return klass->parent_change_state_fn(element, transition);
}

static gboolean gst_videoprep_set_caps(GstBaseTransform* trans, GstCaps* incaps, GstCaps* outcaps) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(trans);
  GstCapsFeatures* ift = NULL;
  // GstStructure* config = NULL;
  GstVideoInfo in_info =
                   {
                       0,
                   },
               out_info = {
                   0,
               };

  // videoprep->input_batch_size = gst::get_batch_size_from_caps(incaps);
  // videoprep->output_batch_size = gst::get_batch_size_from_caps(outcaps);

  GST_DEBUG_OBJECT(videoprep, "set_caps");

  if (!gst_video_info_from_caps(&in_info, incaps)) {
    GST_ERROR("invalid input caps");
    return FALSE;
  }
  videoprep->input_width = GST_VIDEO_INFO_WIDTH(&in_info);
  videoprep->input_height = GST_VIDEO_INFO_HEIGHT(&in_info);
  videoprep->input_fmt = GST_VIDEO_FORMAT_INFO_FORMAT(in_info.finfo);

  if (!gst_video_info_from_caps(&out_info, outcaps)) {
    GST_ERROR("invalid output caps");
    return FALSE;
  }
  // videoprep->output_width = GST_VIDEO_INFO_WIDTH (&videoprep->out_info);
  // videoprep->output_height = GST_VIDEO_INFO_HEIGHT (&videoprep->out_info);
  videoprep->output_fmt = GST_VIDEO_FORMAT_INFO_FORMAT(out_info.finfo);

  ift = gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_NVMM, NULL);
  if (gst_caps_features_is_equal(gst_caps_get_features(outcaps, 0), ift)) {
    videoprep->output_feature = MEM_FEATURE_NVMM;
  } else {
    videoprep->output_feature = MEM_FEATURE_RAW;
  }

  if (gst_caps_features_is_equal(gst_caps_get_features(incaps, 0), ift)) {
    videoprep->input_feature = MEM_FEATURE_NVMM;
  } else {
    videoprep->input_feature = MEM_FEATURE_RAW;
  }
  gst_caps_features_free(ift);

  assert(videoprep->stream);
  videoprep->custom_create_params.m_cudaStream = videoprep->stream;
  videoprep->custom_create_params.m_gpuId = videoprep->gpu_id;
  videoprep->custom_create_params.m_inCaps = incaps;
  videoprep->custom_create_params.m_outCaps = outcaps;

  videoprep->custom_create_params.m_bufferPoolConfig.max_buffers = videoprep->num_output_buffers;
  videoprep->custom_create_params.m_bufferPoolConfig.batch_size = videoprep->num_batch_buffers;
  videoprep->custom_create_params.m_bufferPoolConfig.cuda_mem_type = videoprep->cuda_mem_type;
  videoprep->custom_create_params.m_bufferPoolConfig.gpu_id = videoprep->gpu_id;

  // hm::gst::print_caps_details(incaps);
  // hm::gst::print_caps_details(outcaps);

  absl::Status status = videoprep->priv->PostCapsInit(&videoprep->custom_create_params);
  if (!status.ok()) {
    std::cerr << status << std::endl;
    GST_ERROR("Error on bus: SetInitParams Error");
    return GST_STATE_CHANGE_FAILURE;
  }
#if 0
  // BEGIN BUFFER POOL SETUP
  // Pool Creation
  {
    videoprep->pool = gst_nvds_buffer_pool_new();

    auto config = gst_buffer_pool_get_config(videoprep->pool);

    assert(videoprep->num_output_buffers > 0);
    assert(videoprep->num_batch_buffers > 0);
    // What do we do if they differ?
    // assert(videoprep->num_batch_buffers == videoprep->num_output_buffers);
    // g_print ("in videoconvert caps = %s\n", gst_caps_to_string (outcaps));
    gst_buffer_pool_config_set_params(
        config, outcaps, sizeof(Nv{BufSurface), videoprep->num_output_buffers, videoprep->num_output_buffers);

    gst_structure_set(
        config,
        "memtype",
        G_TYPE_UINT,
        videoprep->cuda_mem_type,
        "gpu-id",
        G_TYPE_UINT,
        videoprep->gpu_id,
        "batch-size",
        G_TYPE_UINT,
        videoprep->num_batch_buffers,
        NULL);

    /* set config for the created buffer pool */
    if (!gst_buffer_pool_set_config(videoprep->pool, config)) {
      GST_WARNING("bufferpool configuration failed");
      return FALSE;
    }

    gboolean is_active = gst_buffer_pool_set_active(videoprep->pool, TRUE);
    if (!is_active) {
      GST_WARNING(" Failed to allocate the buffers inside the output pool");
      return FALSE;
    } else {
      GST_DEBUG(
          " Output buffer pool (%p) successfully created with %d buffers",
          videoprep->pool,
          videoprep->num_batch_buffers);
    }
  }
  // END BUFFER POOL SETUP
#endif
  if (videoprep->priv->AllocateScratchBuffers(videoprep)) {
    GST_ERROR("Error allocating videoprep projection buffers");
    return FALSE;
  }

  gst_base_transform_set_passthrough(trans, FALSE);
  return TRUE;
}

void videoprep_add_surface_meta(GstBuffer* out_gst_buf, int num_filled_surfaces, int source_id) {
  NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)calloc(1, sizeof(NvDewarperSurfaceMeta));

  surface_meta->num_filled_surfaces = num_filled_surfaces;
  surface_meta->source_id = source_id;

  NvDsMeta* meta = NULL;
  meta =
      gst_buffer_add_nvds_meta(out_gst_buf, surface_meta, NULL, videoprep_meta_copy_func, videoprep_meta_release_func);

  meta->meta_type = NVDS_DEWARPER_GST_META;
  meta->gst_to_nvds_meta_transform_func = videoprep_gst_to_nvds_meta_transform_func;
  meta->gst_to_nvds_meta_release_func = videoprep_gst_nvds_meta_release_func;
}

static GstFlowReturn gst_videoprep_submit_input_buffer(GstBaseTransform* btrans, gboolean discont, GstBuffer* inbuf) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(btrans);
  BufferResult result = videoprep->priv->ProcessBuffer(inbuf);
  switch (result) {
    case BufferResult::Buffer_Ok:
    case BufferResult::Buffer_Drop:
    case BufferResult::Buffer_Async:
      return GstFlowReturn::GST_FLOW_OK;
    case BufferResult::Buffer_Error:
    default:
      return GST_FLOW_ERROR;
  }
  assert(false);
}

/**
 * If submit_input_buffer is implemented, it is mandatory to implement
 * generate_output. Buffers are not pushed to the downstream element from here.
 * Return the GstFlowReturn value of the latest pad push so that any error might
 * be caught by the application.
 */
static GstFlowReturn gst_videoprep_generate_output(GstBaseTransform* btrans, GstBuffer** outbuf) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(btrans);
  GstFlowReturn last_flow_ret = videoprep->priv->get_last_flow_ret();
  if (last_flow_ret != GST_FLOW_OK) {
    gst_printerr(
        "videoprep plugin (%s) returning bad flow return value: %d\n", videoprep->plugin_type, (int)last_flow_ret);
  }
  return last_flow_ret;
}

static gboolean gst_videoprep_start(GstBaseTransform* btrans) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(btrans);
  cudaError_t CUerr = cudaSuccess;

  GST_INFO_OBJECT(videoprep, "Using libNVWarp360 version: %s", VIDEOPREP_LIB_VERSION);

  videoprep->frame_num = 0;

  GST_LOG_OBJECT(videoprep, "SETTING CUDA DEVICE = %d in videoprep func=%s\n", videoprep->gpu_id, __func__);
  CUerr = cudaSetDevice(videoprep->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ERROR_OBJECT(videoprep, "cudaSetDevice Failed in %s\n", __func__);
    return FALSE;
  }
  cuda_ck(cudaStreamCreate(&(videoprep->stream)));

  return TRUE;
}

static gboolean gst_videoprep_stop(GstBaseTransform* btrans) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(btrans);

  // std::cout << "gst_videoprep_stop: " << videoprep->plugin_type << std::endl;

  cudaError_t CUerr = cudaSuccess;

  GST_INFO_OBJECT(videoprep, " %s\n", __func__);

  GST_LOG_OBJECT(videoprep, "SETTING CUDA DEVICE = %d in videoprep func=%s\n", videoprep->gpu_id, __func__);
  CUerr = cudaSetDevice(videoprep->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ERROR_OBJECT(videoprep, "cudaSetDevice Failed in %s\n", __func__);
    return FALSE;
  }

  if (videoprep->priv) {
    videoprep->priv->Shutdown();
  }

  if (videoprep->stream) {
    cuda_ck(cudaStreamDestroy(videoprep->stream));
    videoprep->stream = NULL;
  }

  // if (videoprep->pool) {
  //   gst_buffer_pool_set_active(videoprep->pool, FALSE);
  //   gst_object_unref(videoprep->pool);
  //   videoprep->pool = NULL;
  // }

  GST_DEBUG_OBJECT(videoprep, "gst_videoprep_stop");

  return TRUE;
}

static gboolean gst_videoprep_sink_event(GstPad* sinkpad, GstObject* bscope, GstEvent* event) {
  gboolean ret = TRUE;
  GstVideoPrep* videoprep = GST_VIDEOPREP(bscope);

  if (videoprep->priv) {
    ret = videoprep->priv->HandleEvent(event);
    if (!ret)
      return ret;
  }

  ret = videoprep->parent_sink_event_fn(sinkpad, bscope, event);
  if (ret == FALSE) {
    GstState cur_state = GST_STATE_NULL;
    gst_element_get_state(GST_ELEMENT(bscope), &cur_state, NULL, 0);
    if (!(event != NULL || cur_state == GST_STATE_NULL || cur_state == GST_STATE_PAUSED)) {
      g_printerr("sink_event error: %s\n", "sink_event error");
    }
  }
  return ret;
}

/* initialize the videoprep's class */
void gst_videoprep_class_init_base(GstVideoPrepClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseTransformClass* gstbasetransform_class = (GstBaseTransformClass*)klass;

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;

  // Indicates we want to use DS buf api
  g_setenv("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_videoprep_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_videoprep_get_property);
  gobject_class->finalize = GST_DEBUG_FUNCPTR(gst_videoprep_finalize);

  gstbasetransform_class->submit_input_buffer = gst_videoprep_submit_input_buffer;
  gstbasetransform_class->generate_output = gst_videoprep_generate_output;

  gstbasetransform_class->transform_caps = GST_DEBUG_FUNCPTR(gst_videoprep_transform_caps);
  gstbasetransform_class->fixate_caps = GST_DEBUG_FUNCPTR(gst_videoprep_fixate_caps);
  gstbasetransform_class->accept_caps = GST_DEBUG_FUNCPTR(gst_videoprep_accept_caps);
  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR(gst_videoprep_set_caps);

  gstbasetransform_class->start = GST_DEBUG_FUNCPTR(gst_videoprep_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR(gst_videoprep_stop);

  klass->parent_change_state_fn = gstelement_class->change_state;
  gstelement_class->change_state = GST_DEBUG_FUNCPTR(gst_videoprep_change_state);

  klass->parent_state_changed_fn = gstelement_class->state_changed;
  gstelement_class->state_changed = GST_DEBUG_FUNCPTR(gst_videoprep_state_changed);

  // GstPad *sinkpad = GST_PAD(GST_ELEMENT(gstelement_class)->sinkpads->data);
  // GstPad *srcpad = GST_PAD(GST_kpad = ELEMENT(scope)->srcpads->data);

  // scope->parent_sink_event_fn = GST_PAD_EVENTFUNC (sinkpad);

  // gst_pad_set_event_function(sinkpad,
  //   GST_DEBUG_FUNCPTR (gst_nvdsA2Vtemplate_sink_event));

  gstbasetransform_class->passthrough_on_same_caps = FALSE;

  g_object_class_install_property(
      gobject_class,
      PROP_SILENT,
      g_param_spec_boolean("silent", "Silent", "Produce verbose output ?", FALSE, G_PARAM_READWRITE));

  g_object_class_install_property(
      gobject_class,
      PROP_GPU_DEVICE_ID,
      g_param_spec_uint(
          "gpu-id",
          "Set GPU Device ID",
          "Set GPU Device ID",
          0,
          G_MAXUINT,
          DEFAULT_GPU_ID,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_SOURCE_ID,
      g_param_spec_uint(
          "source-id",
          "Set Source / Camera ID",
          "Set Source / Camera ID",
          0,
          G_MAXUINT,
          DEFAULT_SOURCE_ID,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_NUM_OUTPUT_BUFFERS,
      g_param_spec_uint(
          "num-output-buffers",
          "Number of Output Buffers",
          "Number of Output Buffers",
          0,
          G_MAXUINT,
          DEFAULT_NUM_OUTPUT_BUFFERS,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_NUM_BATCH_BUFFERS,
      g_param_spec_uint(
          CONFIG_GROUP_VIDEOPREP_PROPERTY_NUM_BATCH_BUFFERS,
          "Number of Surfaces per output "
          "Buffer",
          "Number of Surfaces per output Buffer",
          0,
          MAX_BUFFERS,
          DEFAULT_NUM_VIDEO_PREPPED_SURFACES,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_OUTPUT_WIDTH,
      g_param_spec_uint(
          CONFIG_GROUP_VIDEOPREP_PROPERTY_OUTPUT_WIDTH,
          "Output Width",
          "Output Width",
          0,
          INT_MAX,
          0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_OUTPUT_HEIGHT,
      g_param_spec_uint(
          CONFIG_GROUP_VIDEOPREP_PROPERTY_OUTPUT_HEIGHT,
          "Output Height",
          "Output Height",
          0,
          INT_MAX,
          0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_CONFIG_FILE,
      g_param_spec_string(
          CONFIG_GROUP_VIDEOPREP_PROPERTY_CONFIG_FILE,
          "Config File",
          "Config File",
          NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_PLUGIN_TYPE,
      g_param_spec_string(
          CONFIG_GROUP_VIDEOPREP_PLUGIN_TYPE,
          "Plugin Type",
          "Plugin Type",
          NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_PLUGIN_PRIVATE_CONFIG,
      g_param_spec_string(
          CONFIG_GROUP_VIDEOPREP_PLUGIN_PRIVATE_CONFIG,
          "Plugin Privatye Config",
          "Plugin Privatye Config \"key1=val1;key2=val2;...\"",
          NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_POST_STITCH_ROTATE_DEGREES,
      g_param_spec_double(
          "post-stitch-rotate-degrees",
          "Post-stitch rotate degrees",
          "Runtime rotation applied by hmstitcher after stitching",
          -360.0,
          360.0,
          0.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_FIXED_EDGE_ROTATION_ANGLE,
      g_param_spec_double(
          "fixed-edge-rotation-angle",
          "Fixed edge rotation angle",
          "Runtime fixed-edge rotation angle consumed by playcropper and vpplaytracker",
          -180.0,
          180.0,
          10.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_FIXED_EDGE_ROTATION_ANGLE_LEFT,
      g_param_spec_double(
          "fixed-edge-rotation-angle-left",
          "Left fixed edge rotation angle",
          "Runtime fixed-edge rotation angle used on the left half of the panorama",
          -180.0,
          180.0,
          10.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_FIXED_EDGE_ROTATION_ANGLE_RIGHT,
      g_param_spec_double(
          "fixed-edge-rotation-angle-right",
          "Right fixed edge rotation angle",
          "Runtime fixed-edge rotation angle used on the right half of the panorama",
          -180.0,
          180.0,
          10.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_DYNAMIC_ACCELERATION_SCALING,
      g_param_spec_double(
          "dynamic-acceleration-scaling",
          "Dynamic acceleration scaling",
          "Runtime dynamic acceleration scaling consumed by vpplaytracker",
          0.0,
          100.0,
          1.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_PREVIEW_OVERLAY_FLAGS,
      g_param_spec_uint(
          "preview-overlay-flags",
          "Preview overlay flags",
          "Preview-only snapshot content and Program transform bitmask",
          0,
          7,
          0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_HIGH_BIT_DEPTH,
      g_param_spec_boolean(
          "high-bit-depth",
          "High-bit-depth stitching",
          "Accept RGB10A2 and keep RGB channels in FP16 until the final stitched RGBA8 conversion",
          FALSE,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_SHADOW_LIFT,
      g_param_spec_double(
          "shadow-lift",
          "Bring up shadows",
          "VIDEO-style monotone shadow lift percentage consumed by playcropper",
          0.0,
          100.0,
          0.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_SHADOW_LIFT_BLACK_POINT,
      g_param_spec_boolean(
          "shadow-lift-black-point",
          "Lift shadow black point",
          "Allow the playcropper shadow control to raise exact black with a smooth toe rolloff",
          FALSE,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_EXPOSURE,
      g_param_spec_double(
          "exposure",
          "Exposure",
          "Uniform exposure gain; 1.0 applies +0.5 stop (sqrt(2) gain) and clips at white",
          0.0,
          1.3,
          0.0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_RUNTIME_TUNING_CONFIG_FILE,
      g_param_spec_string(
          "runtime-tuning-config-file",
          "Runtime play tracker tuning config",
          "State-preserving play tracker tuning configuration",
          NULL,
          GParamFlags(G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_CANCEL_PENDING_WORK,
      g_param_spec_boolean(
          "cancel-pending-work",
          "Cancel pending work",
          "Request cooperative cancellation of long-running plugin work",
          FALSE,
          GParamFlags(G_PARAM_WRITABLE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_PLAYING)));

  g_object_class_install_property(
      gobject_class,
      PROP_LAST_PROPERTY_SET_OK,
      g_param_spec_boolean(
          "last-property-set-ok",
          "Last property set ok",
          "Whether the most recent videoprep private property update was accepted",
          TRUE,
          GParamFlags(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));

  PROP_NVBUF_MEMORY_TYPE_INSTALL(gobject_class);
  PROP_INTERPOLATION_METHOD_INSTALL(gobject_class);

  gst_element_class_set_details_simple(
      gstelement_class,
      "videoprep",
      "videoprep",
      "Gstreamer VIDEOPREP Element",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");

  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&src_factory));
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&sink_factory));
}

static void gst_videoprep_class_init(GstVideoPrepClass* klass) {
  gst_videoprep_class_init_base(klass);
}

/* initialize the new element
 * instantiate pads and add them to element
 * set pad calback functions
 * initialize instance structure
 */
void gst_videoprep_init_base(GstVideoPrep* videoprep) {
  videoprep->sinkcaps = gst_static_pad_template_get_caps(&sink_factory);
  videoprep->srccaps = gst_static_pad_template_get_caps(&src_factory);

  videoprep->silent = FALSE;
  // videoprep->pool = NULL;

  videoprep->num_batch_buffers = DEFAULT_NUM_VIDEO_PREPPED_SURFACES;
  videoprep->cuda_mem_type = NVBUF_MEM_DEFAULT;
  videoprep->interpolation_method = NvBufSurfTransformInter_Default;

  // TODO: If CSV is not given then we should not check this
  videoprep->config_file = NULL;
  assert(!videoprep->plugin_type);
  videoprep->plugin_type = NULL; // strdup("videoprep");
  videoprep->post_stitch_rotate_degrees = 0.0;
  videoprep->fixed_edge_rotation_angle = 10.0;
  videoprep->fixed_edge_rotation_angle_left = 10.0;
  videoprep->fixed_edge_rotation_angle_right = 10.0;
  videoprep->dynamic_acceleration_scaling = 1.0;
  videoprep->preview_overlay_flags = 0;
  videoprep->high_bit_depth = FALSE;
  videoprep->shadow_lift = 0.0;
  videoprep->shadow_lift_black_point = FALSE;
  videoprep->exposure = 0.0;
  videoprep->last_property_set_ok = TRUE;
  videoprep->post_stitch_rotate_degrees_set = FALSE;
  videoprep->fixed_edge_rotation_angle_set = FALSE;
  videoprep->fixed_edge_rotation_angle_left_set = FALSE;
  videoprep->fixed_edge_rotation_angle_right_set = FALSE;
  videoprep->dynamic_acceleration_scaling_set = FALSE;
  videoprep->high_bit_depth_set = FALSE;
  videoprep->shadow_lift_set = FALSE;
  videoprep->shadow_lift_black_point_set = FALSE;
  videoprep->exposure_set = FALSE;
  videoprep->property_set_sequence = 0;
  videoprep->plugin_private_config_sequence = 0;
  videoprep->post_stitch_rotate_degrees_sequence = 0;
  videoprep->fixed_edge_rotation_angle_sequence = 0;
  videoprep->fixed_edge_rotation_angle_left_sequence = 0;
  videoprep->fixed_edge_rotation_angle_right_sequence = 0;
  videoprep->dynamic_acceleration_scaling_sequence = 0;
  videoprep->high_bit_depth_sequence = 0;
  videoprep->shadow_lift_sequence = 0;
  videoprep->shadow_lift_black_point_sequence = 0;
  videoprep->exposure_sequence = 0;
  videoprep->priv_factory = new VideoPrepLibrary_Factory();

  videoprep->num_output_buffers = DEFAULT_NUM_OUTPUT_BUFFERS;

  // videoprep->dump_frames = DEFAULT_DEWARP_DUMP_FRAMES;

  videoprep->output_width = DEFAULT_DEWARP_OUTPUT_WIDTH;
  videoprep->output_height = DEFAULT_DEWARP_OUTPUT_HEIGHT;

  // GstElement *elem = GST_ELEMENT(videoprep);
  // std::cout << "hwfh34f" << std::endl;
  //  gstelement_class->state_changed = GST_DEBUG_FUNCPTR(gst_videoprep_state_changed);

  GstPad* sinkpad = GST_PAD(GST_ELEMENT(videoprep)->sinkpads->data);
  // GstPad *srcpad = GST_PAD(GST_ELEMENT(videoprep)->srcpads->data);

  videoprep->parent_sink_event_fn = GST_PAD_EVENTFUNC(sinkpad);

  gst_pad_set_event_function(sinkpad, GST_DEBUG_FUNCPTR(gst_videoprep_sink_event));
}

static void gst_videoprep_finalize(GObject* object) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(object);
  if (videoprep->output) {
    cuda_ck(cudaFreeHost(videoprep->output));
    videoprep->output = NULL;
  }

  if (videoprep->priv_factory) {
    delete videoprep->priv_factory;
    videoprep->priv_factory = nullptr;
  }

  if (videoprep->priv) {
    videoprep->priv->scratch_buffers.clear();
    if (videoprep->priv) {
      // videoprep->priv->Shutdown();
      delete videoprep->priv;
      videoprep->priv = NULL;
    }
  }
  if (videoprep->config_file)
    g_free(videoprep->config_file);
  if (videoprep->plugin_type)
    g_free(videoprep->plugin_type);
}

static void gst_videoprep_init(GstVideoPrep* videoprep) {
  gst_videoprep_init_base(videoprep);
}

bool VideoPrepPriv::SetPrivateConfig(const char* config_string) {
  if (!config_string) {
    return true;
  }
  bool ok = true;
  std::string all_string = config_string;
  std::vector<std::string> kv_pairs = absl::StrSplit(std::move(all_string), ';');
  for (const std::string& kv : kv_pairs) {
    std::vector<std::string> kv_tokens = absl::StrSplit(kv, '=');
    if (!kv_tokens.empty() && !kv_tokens[0].empty()) {
      if (kv_tokens.size() != 2) {
        g_printerr("Error parsing key-value pair: \"%s\"\n", kv.c_str());
        ok = false;
        continue;
      }
      ok = SetProperty(Property(kv_tokens.at(0), kv_tokens.at(1))) && ok;
    }
  }
  return ok;
}

static void gst_videoprep_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(object);
  auto set_priv_property = [videoprep](const char* key, const std::string& val) -> bool {
    if (videoprep->priv) {
      videoprep->last_property_set_ok = videoprep->priv->SetProperty(Property(key, val));
      if (!videoprep->last_property_set_ok) {
        g_printerr("videoprep property update rejected: %s=%s\n", key, val.c_str());
      }
    } else {
      videoprep->last_property_set_ok = TRUE;
    }
    return videoprep->last_property_set_ok;
  };
  switch (prop_id) {
    case PROP_SILENT:
      videoprep->silent = g_value_get_boolean(value);
      break;
      PROPERTY_SET_CASE(PROP_GPU_DEVICE_ID, videoprep->gpu_id)
      PROPERTY_SET_CASE(PROP_SOURCE_ID, videoprep->source_id)
      PROPERTY_SET_CASE(PROP_NUM_OUTPUT_BUFFERS, videoprep->num_output_buffers)
      PROPERTY_SET_CASE(PROP_NUM_BATCH_BUFFERS, videoprep->num_batch_buffers)
      PROPERTY_SET_CASE(PROP_NVBUF_MEMORY_TYPE, videoprep->cuda_mem_type)
      PROPERTY_SET_CASE(PROP_INTERPOLATION_METHOD, videoprep->interpolation_method);
      PROPERTY_SET_CASE(PROP_OUTPUT_WIDTH, videoprep->output_width);
      PROPERTY_SET_CASE(PROP_OUTPUT_HEIGHT, videoprep->output_height);
      PROPERTY_SET_CASE(PROP_PLUGIN_TYPE, videoprep->plugin_type);
    case PROP_CONFIG_FILE: {
      gchar* previous_config_file = videoprep->config_file ? g_strdup(videoprep->config_file) : nullptr;
      hm::gst::set_value(videoprep->config_file, value);
      if (videoprep->config_file && !set_priv_property("config-file", videoprep->config_file)) {
        g_free(videoprep->config_file);
        videoprep->config_file = previous_config_file;
        previous_config_file = nullptr;
      }
      g_free(previous_config_file);
      break;
    }
    case PROP_PLUGIN_PRIVATE_CONFIG:
      hm::gst::set_value(videoprep->plugin_private_config, value);
      videoprep->plugin_private_config_sequence = ++videoprep->property_set_sequence;
      if (videoprep->priv) {
        videoprep->last_property_set_ok = videoprep->priv->SetPrivateConfig(videoprep->plugin_private_config);
      }
      break;
    case PROP_POST_STITCH_ROTATE_DEGREES: {
      const gdouble previous = videoprep->post_stitch_rotate_degrees;
      const gboolean previous_set = videoprep->post_stitch_rotate_degrees_set;
      const guint previous_sequence = videoprep->post_stitch_rotate_degrees_sequence;
      videoprep->post_stitch_rotate_degrees = g_value_get_double(value);
      videoprep->post_stitch_rotate_degrees_set = TRUE;
      videoprep->post_stitch_rotate_degrees_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("post-stitch-rotate-degrees", std::to_string(videoprep->post_stitch_rotate_degrees))) {
        videoprep->post_stitch_rotate_degrees = previous;
        videoprep->post_stitch_rotate_degrees_set = previous_set;
        videoprep->post_stitch_rotate_degrees_sequence = previous_sequence;
      }
      break;
    }
    case PROP_FIXED_EDGE_ROTATION_ANGLE: {
      const gdouble previous = videoprep->fixed_edge_rotation_angle;
      const gboolean previous_set = videoprep->fixed_edge_rotation_angle_set;
      const guint previous_sequence = videoprep->fixed_edge_rotation_angle_sequence;
      videoprep->fixed_edge_rotation_angle = g_value_get_double(value);
      videoprep->fixed_edge_rotation_angle_set = TRUE;
      videoprep->fixed_edge_rotation_angle_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("fixed-edge-rotation-angle", std::to_string(videoprep->fixed_edge_rotation_angle))) {
        videoprep->fixed_edge_rotation_angle = previous;
        videoprep->fixed_edge_rotation_angle_set = previous_set;
        videoprep->fixed_edge_rotation_angle_sequence = previous_sequence;
      }
      break;
    }
    case PROP_FIXED_EDGE_ROTATION_ANGLE_LEFT: {
      const gdouble previous = videoprep->fixed_edge_rotation_angle_left;
      const gboolean previous_set = videoprep->fixed_edge_rotation_angle_left_set;
      const guint previous_sequence = videoprep->fixed_edge_rotation_angle_left_sequence;
      videoprep->fixed_edge_rotation_angle_left = g_value_get_double(value);
      videoprep->fixed_edge_rotation_angle_left_set = TRUE;
      videoprep->fixed_edge_rotation_angle_left_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property(
              "fixed-edge-rotation-angle-left", std::to_string(videoprep->fixed_edge_rotation_angle_left))) {
        videoprep->fixed_edge_rotation_angle_left = previous;
        videoprep->fixed_edge_rotation_angle_left_set = previous_set;
        videoprep->fixed_edge_rotation_angle_left_sequence = previous_sequence;
      }
      break;
    }
    case PROP_FIXED_EDGE_ROTATION_ANGLE_RIGHT: {
      const gdouble previous = videoprep->fixed_edge_rotation_angle_right;
      const gboolean previous_set = videoprep->fixed_edge_rotation_angle_right_set;
      const guint previous_sequence = videoprep->fixed_edge_rotation_angle_right_sequence;
      videoprep->fixed_edge_rotation_angle_right = g_value_get_double(value);
      videoprep->fixed_edge_rotation_angle_right_set = TRUE;
      videoprep->fixed_edge_rotation_angle_right_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property(
              "fixed-edge-rotation-angle-right", std::to_string(videoprep->fixed_edge_rotation_angle_right))) {
        videoprep->fixed_edge_rotation_angle_right = previous;
        videoprep->fixed_edge_rotation_angle_right_set = previous_set;
        videoprep->fixed_edge_rotation_angle_right_sequence = previous_sequence;
      }
      break;
    }
    case PROP_DYNAMIC_ACCELERATION_SCALING: {
      const gdouble previous = videoprep->dynamic_acceleration_scaling;
      const gboolean previous_set = videoprep->dynamic_acceleration_scaling_set;
      const guint previous_sequence = videoprep->dynamic_acceleration_scaling_sequence;
      videoprep->dynamic_acceleration_scaling = g_value_get_double(value);
      videoprep->dynamic_acceleration_scaling_set = TRUE;
      videoprep->dynamic_acceleration_scaling_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("dynamic-acceleration-scaling", std::to_string(videoprep->dynamic_acceleration_scaling))) {
        videoprep->dynamic_acceleration_scaling = previous;
        videoprep->dynamic_acceleration_scaling_set = previous_set;
        videoprep->dynamic_acceleration_scaling_sequence = previous_sequence;
      }
      break;
    }
    case PROP_PREVIEW_OVERLAY_FLAGS: {
      const guint previous = videoprep->preview_overlay_flags;
      videoprep->preview_overlay_flags = g_value_get_uint(value);
      if (!set_priv_property("preview-overlay-flags", std::to_string(videoprep->preview_overlay_flags))) {
        videoprep->preview_overlay_flags = previous;
      }
      break;
    }
    case PROP_HIGH_BIT_DEPTH: {
      const gboolean previous = videoprep->high_bit_depth;
      const gboolean previous_set = videoprep->high_bit_depth_set;
      const guint previous_sequence = videoprep->high_bit_depth_sequence;
      videoprep->high_bit_depth = g_value_get_boolean(value);
      videoprep->high_bit_depth_set = TRUE;
      videoprep->high_bit_depth_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("high-bit-depth", videoprep->high_bit_depth ? "1" : "0")) {
        videoprep->high_bit_depth = previous;
        videoprep->high_bit_depth_set = previous_set;
        videoprep->high_bit_depth_sequence = previous_sequence;
      }
      break;
    }
    case PROP_SHADOW_LIFT: {
      const gdouble previous = videoprep->shadow_lift;
      const gboolean previous_set = videoprep->shadow_lift_set;
      const guint previous_sequence = videoprep->shadow_lift_sequence;
      videoprep->shadow_lift = g_value_get_double(value);
      videoprep->shadow_lift_set = TRUE;
      videoprep->shadow_lift_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("shadow-lift", std::to_string(videoprep->shadow_lift))) {
        videoprep->shadow_lift = previous;
        videoprep->shadow_lift_set = previous_set;
        videoprep->shadow_lift_sequence = previous_sequence;
      }
      break;
    }
    case PROP_SHADOW_LIFT_BLACK_POINT: {
      const gboolean previous = videoprep->shadow_lift_black_point;
      const gboolean previous_set = videoprep->shadow_lift_black_point_set;
      const guint previous_sequence = videoprep->shadow_lift_black_point_sequence;
      videoprep->shadow_lift_black_point = g_value_get_boolean(value);
      videoprep->shadow_lift_black_point_set = TRUE;
      videoprep->shadow_lift_black_point_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("shadow-lift-black-point", videoprep->shadow_lift_black_point ? "1" : "0")) {
        videoprep->shadow_lift_black_point = previous;
        videoprep->shadow_lift_black_point_set = previous_set;
        videoprep->shadow_lift_black_point_sequence = previous_sequence;
      }
      break;
    }
    case PROP_EXPOSURE: {
      const gdouble previous = videoprep->exposure;
      const gboolean previous_set = videoprep->exposure_set;
      const guint previous_sequence = videoprep->exposure_sequence;
      videoprep->exposure = g_value_get_double(value);
      videoprep->exposure_set = TRUE;
      videoprep->exposure_sequence = ++videoprep->property_set_sequence;
      if (!set_priv_property("exposure", std::to_string(videoprep->exposure))) {
        videoprep->exposure = previous;
        videoprep->exposure_set = previous_set;
        videoprep->exposure_sequence = previous_sequence;
      }
      break;
    }
    case PROP_RUNTIME_TUNING_CONFIG_FILE: {
      const gchar* config_file = g_value_get_string(value);
      videoprep->last_property_set_ok =
          config_file && *config_file && set_priv_property("runtime-tuning-config-file", config_file);
      break;
    }
    case PROP_CANCEL_PENDING_WORK:
      if (g_value_get_boolean(value)) {
        set_priv_property("cancel-pending-work", "1");
      }
      break;
    case PROP_LAST_PROPERTY_SET_OK:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
  if (prop_id == PROP_PLUGIN_TYPE) {
    std::cout << "plugin_type: " << videoprep->plugin_type << std::endl;
  }
}

static bool plugin_private_config_has_key(const gchar* config_string, const char* key) {
  if (!config_string || !key || !*key) {
    return false;
  }
  std::vector<std::string> kv_pairs = absl::StrSplit(std::string(config_string), ';');
  for (const std::string& kv : kv_pairs) {
    std::vector<std::string> kv_tokens = absl::StrSplit(kv, '=');
    if (!kv_tokens.empty() && kv_tokens[0] == key) {
      return true;
    }
  }
  return false;
}

static bool typed_property_wins_over_private_config(GstVideoPrep* videoprep, guint typed_sequence, const char* key) {
  return typed_sequence > videoprep->plugin_private_config_sequence ||
      !plugin_private_config_has_key(videoprep->plugin_private_config, key);
}

static bool gst_videoprep_apply_typed_properties(GstVideoPrep* videoprep) {
  if (!videoprep || !videoprep->priv) {
    return true;
  }
  bool ok = true;
  if (videoprep->post_stitch_rotate_degrees_set &&
      typed_property_wins_over_private_config(
          videoprep, videoprep->post_stitch_rotate_degrees_sequence, "post-stitch-rotate-degrees")) {
    ok = videoprep->priv->SetProperty(
             Property("post-stitch-rotate-degrees", std::to_string(videoprep->post_stitch_rotate_degrees))) &&
        ok;
  }
  if (videoprep->fixed_edge_rotation_angle_set &&
      typed_property_wins_over_private_config(
          videoprep, videoprep->fixed_edge_rotation_angle_sequence, "fixed-edge-rotation-angle")) {
    ok = videoprep->priv->SetProperty(
             Property("fixed-edge-rotation-angle", std::to_string(videoprep->fixed_edge_rotation_angle))) &&
        ok;
  }
  if (videoprep->fixed_edge_rotation_angle_left_set &&
      typed_property_wins_over_private_config(
          videoprep, videoprep->fixed_edge_rotation_angle_left_sequence, "fixed-edge-rotation-angle-left")) {
    ok = videoprep->priv->SetProperty(
             Property("fixed-edge-rotation-angle-left", std::to_string(videoprep->fixed_edge_rotation_angle_left))) &&
        ok;
  }
  if (videoprep->fixed_edge_rotation_angle_right_set &&
      typed_property_wins_over_private_config(
          videoprep, videoprep->fixed_edge_rotation_angle_right_sequence, "fixed-edge-rotation-angle-right")) {
    ok = videoprep->priv->SetProperty(
             Property("fixed-edge-rotation-angle-right", std::to_string(videoprep->fixed_edge_rotation_angle_right))) &&
        ok;
  }
  if (videoprep->dynamic_acceleration_scaling_set &&
      typed_property_wins_over_private_config(
          videoprep, videoprep->dynamic_acceleration_scaling_sequence, "dynamic-acceleration-scaling")) {
    ok = videoprep->priv->SetProperty(
             Property("dynamic-acceleration-scaling", std::to_string(videoprep->dynamic_acceleration_scaling))) &&
        ok;
  }
  if (videoprep->plugin_type && std::string_view(videoprep->plugin_type) == "vpplaytracker") {
    ok = videoprep->priv->SetProperty(
             Property("preview-overlay-flags", std::to_string(videoprep->preview_overlay_flags))) &&
        ok;
  }
  if (videoprep->high_bit_depth_set &&
      typed_property_wins_over_private_config(videoprep, videoprep->high_bit_depth_sequence, "high-bit-depth")) {
    ok = videoprep->priv->SetProperty(Property("high-bit-depth", videoprep->high_bit_depth ? "1" : "0")) && ok;
  }
  if (videoprep->shadow_lift_set &&
      typed_property_wins_over_private_config(videoprep, videoprep->shadow_lift_sequence, "shadow-lift")) {
    ok = videoprep->priv->SetProperty(Property("shadow-lift", std::to_string(videoprep->shadow_lift))) && ok;
  }
  if (videoprep->shadow_lift_black_point_set &&
      typed_property_wins_over_private_config(
          videoprep, videoprep->shadow_lift_black_point_sequence, "shadow-lift-black-point")) {
    ok = videoprep->priv->SetProperty(
             Property("shadow-lift-black-point", videoprep->shadow_lift_black_point ? "1" : "0")) &&
        ok;
  }
  if (videoprep->exposure_set &&
      typed_property_wins_over_private_config(videoprep, videoprep->exposure_sequence, "exposure")) {
    ok = videoprep->priv->SetProperty(Property("exposure", std::to_string(videoprep->exposure))) && ok;
  }
  videoprep->last_property_set_ok = ok;
  return ok;
}

static void gst_videoprep_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(object);
  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean(value, videoprep->silent);
      break;
      PROPERTY_GET_CASE(PROP_GPU_DEVICE_ID, videoprep->gpu_id)
      PROPERTY_GET_CASE(PROP_SOURCE_ID, videoprep->source_id)
      PROPERTY_GET_CASE(PROP_NUM_OUTPUT_BUFFERS, videoprep->num_output_buffers)
      PROPERTY_GET_CASE(PROP_NUM_BATCH_BUFFERS, videoprep->num_batch_buffers)
      PROPERTY_GET_CASE(PROP_NVBUF_MEMORY_TYPE, videoprep->cuda_mem_type)
      PROPERTY_GET_CASE(PROP_INTERPOLATION_METHOD, videoprep->interpolation_method);
      PROPERTY_GET_CASE(PROP_OUTPUT_WIDTH, videoprep->output_width);
      PROPERTY_GET_CASE(PROP_OUTPUT_HEIGHT, videoprep->output_height);
      PROPERTY_GET_CASE(PROP_PLUGIN_TYPE, videoprep->plugin_type);
      PROPERTY_GET_CASE(PROP_PLUGIN_PRIVATE_CONFIG, videoprep->plugin_private_config);
      PROPERTY_GET_CASE(PROP_CONFIG_FILE, videoprep->config_file);
    case PROP_POST_STITCH_ROTATE_DEGREES:
      g_value_set_double(value, videoprep->post_stitch_rotate_degrees);
      break;
    case PROP_FIXED_EDGE_ROTATION_ANGLE:
      g_value_set_double(value, videoprep->fixed_edge_rotation_angle);
      break;
    case PROP_FIXED_EDGE_ROTATION_ANGLE_LEFT:
      g_value_set_double(value, videoprep->fixed_edge_rotation_angle_left);
      break;
    case PROP_FIXED_EDGE_ROTATION_ANGLE_RIGHT:
      g_value_set_double(value, videoprep->fixed_edge_rotation_angle_right);
      break;
    case PROP_DYNAMIC_ACCELERATION_SCALING:
      g_value_set_double(value, videoprep->dynamic_acceleration_scaling);
      break;
    case PROP_PREVIEW_OVERLAY_FLAGS:
      g_value_set_uint(value, videoprep->preview_overlay_flags);
      break;
    case PROP_HIGH_BIT_DEPTH:
      g_value_set_boolean(value, videoprep->high_bit_depth);
      break;
    case PROP_SHADOW_LIFT:
      g_value_set_double(value, videoprep->shadow_lift);
      break;
    case PROP_SHADOW_LIFT_BLACK_POINT:
      g_value_set_boolean(value, videoprep->shadow_lift_black_point);
      break;
    case PROP_EXPOSURE:
      g_value_set_double(value, videoprep->exposure);
      break;
    case PROP_RUNTIME_TUNING_CONFIG_FILE:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
    case PROP_LAST_PROPERTY_SET_OK:
      g_value_set_boolean(value, videoprep->last_property_set_ok);
      break;
    // Add additional cases here...
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

} // namespace videoprep
} // namespace hm
