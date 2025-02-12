#include "gstvideoprep.h"
#include "gstnvdsbufferpool.h"

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <npp.h>
#include <nvbufsurface.h>
#include <cmath>
#include <iostream>
#include "gst-nvcommon.h"
#include "gstnvdsbufferpool.h"
#include "gstnvdsmeta.h"
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
#include "nvbufsurface.h"

#if defined(__aarch64__)
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include "cudaEGL.h"
#endif

#define DEFAULT_NUM_VIDEO_PREPPED_SURFACES (4)
#define DEFAULT_DEWARP_DUMP_FRAMES 0
#define DEFAULT_DEWARP_OUTPUT_WIDTH 960
#define DEFAULT_DEWARP_OUTPUT_HEIGHT 752

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

// Helper macros for alignment
#define NVBUF_ALIGN_VAL (256)
#define NVBUF_ALIGN_PITCH(pitch, align_val) ((pitch % align_val == 0) ? pitch : ((pitch / align_val + 1) * align_val))
#define NVBUF_PLATFORM_ALIGNED_PITCH(pitch) NVBUF_ALIGN_PITCH(pitch, NVBUF_ALIGN_VAL)

// #define SCRATCH_USE_ALIGNED_PITCH

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
  PROP_DEWARP_LIB_VERSION,
  PROP_NUM_BATCH_BUFFERS,
  PROP_NVBUF_MEMORY_TYPE,
  PROP_INTERPOLATION_METHOD,
  PROP_SILENT,
};

static void gst_videoprep_finalize(GObject* object);

static const gchar* print_pretty_time(gchar* ts_str, gsize ts_str_len, GstClockTime ts) {
  if (ts == GST_CLOCK_TIME_NONE)
    return "none";

  g_snprintf(ts_str, ts_str_len, "%" GST_TIME_FORMAT, GST_TIME_ARGS(ts));
  return ts_str;
}

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
        "RGBA }")));

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

static gpointer videoprep_gst_to_nvds_meta_ransform_func(gpointer data, gpointer user_data) {
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

  /* get all the formats we can handle on this pad */
  if (direction == GST_PAD_SINK)
    allowed = videoprep->sinkcaps;
  else
    allowed = videoprep->srccaps;

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

  out_width = videoprep->output_width;
  out_height = videoprep->output_height;

  gst_structure_remove_fields(outs, "width", "height", NULL);

  gst_structure_set(outs, "width", G_TYPE_INT, out_width, "height", G_TYPE_INT, out_height, NULL);

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
  if (direction == GST_PAD_SINK) {
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
        NULL);
    feature = gst_caps_features_new("memory:NVMM", NULL);
    gst_caps_set_features(new_caps, 0, feature);
  }
  if (direction == GST_PAD_SRC) {
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

// static gint gst_videoprep_allocate_projection_buffers(GstVideoPrep* videoprep) {
//   // std::vector<VideoPrepParams>::iterator iter;
//   // guint i = 0;
//   cudaError_t cudaErr;

//   cudaErr = cudaSetDevice(videoprep->gpu_id);
//   if (cudaErr != cudaSuccess) {
//     printf("\n *** Unable to set device in %s Line %d\n", __func__, __LINE__);
//     return cudaErr;
//   }

// #if 1
//   static bool ranthis = false;
//   (void)ranthis;
//   assert(!ranthis);
//   ranthis = true;

//   hm::WHDims src_size{.width = (FloatValue)videoprep->input_width, .height = (FloatValue)videoprep->input_height};
//   constexpr FloatValue out_ar = 16.0 / 9.0;
//   FloatValue virt_out_width = ((FloatValue)videoprep->input_height) * out_ar;
//   hm::WHDims output_size{.width = virt_out_width, .height = (FloatValue)videoprep->input_height};

//   videoprep->pre_rotate_size = get_box_size_necessary_for_rotations(src_size, output_size);

// #endif
//   constexpr size_t kBytesPerPixel = 4;
// #ifdef SCRATCH_USE_ALIGNED_PITCH
//   size_t pitch = NVBUF_PLATFORM_ALIGNED_PITCH((size_t)videoprep->pre_rotate_size.width * kBytesPerPixel);
// #else
//   // No alignment for simpler functions that cant handle aligned pitch
//   size_t pitch = (size_t)videoprep->pre_rotate_size.width * kBytesPerPixel;
// #endif
//   constexpr size_t kNumScratchBuffers = 2;
//   for (size_t i = 0; i < kNumScratchBuffers; ++i) {
//     void* surface_ptr = nullptr;
//     cuda_ck(cudaMalloc(&surface_ptr, pitch * (size_t)videoprep->pre_rotate_size.height));
//     // cuda_ck(cudaMallocHost(&surface_ptr, pitch * (size_t)videoprep->pre_rotate_size.height));

//     videoprep->priv->scratch_buffers.add_surface(
//         surface_ptr,
//         videoprep->pre_rotate_size.width,
//         videoprep->pre_rotate_size.height,
//         pitch,
//         kBytesPerPixel,
//         /*owns=*/true);
//   }
//   return 0;
// }

static gboolean gst_videoprep_set_caps(GstBaseTransform* trans, GstCaps* incaps, GstCaps* outcaps) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(trans);
  GstCapsFeatures* ift = NULL;
  GstStructure* config = NULL;
  GstVideoInfo in_info = {}, out_info = {};

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

  // Pool Creation
  {
    videoprep->pool = gst_nvds_buffer_pool_new();

    config = gst_buffer_pool_get_config(videoprep->pool);

    assert(videoprep->num_output_buffers > 0);
    assert(videoprep->num_batch_buffers > 0);
    // What do we do if they differ?
    assert(videoprep->num_batch_buffers == videoprep->num_output_buffers);
    // g_print ("in videoconvert caps = %s\n", gst_caps_to_string (outcaps));
    gst_buffer_pool_config_set_params(
        config, outcaps, sizeof(NvBufSurface), videoprep->num_output_buffers, videoprep->num_output_buffers);

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
  // if (!videoprep->aisle_calibrationfile_set || !videoprep->spot_calibrationfile_set) {
  //  Non-CVS Case
  assert(!videoprep->priv);
  // videoprep->priv = new VideoPrepPriv(videoprep->gpu_id, videoprep->num_batch_buffers);
  GObject* object = G_OBJECT(trans);
  assert(object);
  // TODO: remove need for it to be anything but the base type
  videoprep->priv = dynamic_cast<VideoPrepPriv*>(videoprep->priv_factory->CreateCustomAlgoCtx(
      videoprep->plugin_type, object, videoprep->gpu_id, videoprep->num_batch_buffers));
  if (!videoprep->priv) {
    GST_ERROR("Unable to create plugin type %s", videoprep->plugin_type);
    return FALSE;
  }
  //gst_videoprep_allocate_projection_buffers(videoprep);
  if (videoprep->priv->AllocateScratchBuffers(videoprep)) {
    GST_ERROR("Error allocating videoprep projection buffers");
    return FALSE;
  }

  gst_base_transform_set_passthrough(trans, FALSE);
  return TRUE;
}

void inspect_nvbufsurface_dtype(GstBuffer* buffer) {
  // Map the GstBuffer to retrieve the NvBufSurface
  GstMapInfo map_info;
  if (!gst_buffer_map(buffer, &map_info, GST_MAP_READ)) {
    GST_ERROR("Failed to map GstBuffer.");
    return;
  }

  NvBufSurface* nvbuf_surface = (NvBufSurface*)map_info.data;
  if (!nvbuf_surface) {
    GST_ERROR("NvBufSurface is null.");
    gst_buffer_unmap(buffer, &map_info);
    return;
  }

  // Iterate through surfaces in the NvBufSurface
  for (uint32_t i = 0; i < nvbuf_surface->numFilled; i++) {
    NvBufSurfaceParams& surface_params = nvbuf_surface->surfaceList[i];
    GST_INFO(
        "Surface %d: width=%d, height=%d, pitch=%d",
        i,
        surface_params.width,
        surface_params.height,
        surface_params.pitch);

    switch (surface_params.colorFormat) {
      case NVBUF_COLOR_FORMAT_RGBA:
        GST_INFO("Surface %d has color format: NVBUF_COLOR_FORMAT_RGBA (dtype: uint8_t).", i);
        break;

      case NVBUF_COLOR_FORMAT_NV12:
        GST_INFO("Surface %d has color format: NVBUF_COLOR_FORMAT_NV12 (dtype: uint8_t, YUV 4:2:0).", i);
        break;

      case NVBUF_COLOR_FORMAT_UYVY:
        GST_INFO("Surface %d has color format: NVBUF_COLOR_FORMAT_UYVY (dtype: uint8_t, YUV 4:2:2).", i);
        break;

      default:
        GST_WARNING("Surface %d has an unsupported or unknown color format: %d", i, surface_params.colorFormat);
        break;
    }
  }

  // Unmap the buffer
  gst_buffer_unmap(buffer, &map_info);
}

// static gboolean gst_videoprep_render(GstBaseTransform* trans, GstBuffer* audio, GstVideoFrame* video) {
//   GstVideoPrep* videoprep = GST_VIDEOPREP(trans);
//   GstElement* element = GST_ELEMENT(trans);
//   GstPad* srcpad = GST_PAD(element->srcpads->data);
//   GstFlowReturn ret;
//   BufferResult ret_process_buffer = BufferResult::Buffer_Ok;

//   GstBuffer *outbuf = NULL, *temp = NULL;

//   ret = gst_buffer_pool_acquire_buffer(videoprep->pool, &outbuf, NULL);
//   if (ret != GST_FLOW_OK) {
//     GST_ERROR_ON_BUS("failed to activate bufferpool", "failed to activate bufferpool");
//     return false;
//   }

//   GST_BUFFER_PTS(outbuf) = GST_BUFFER_PTS(video->buffer);
//   GST_BUFFER_DURATION(outbuf) = GST_BUFFER_DURATION(video->buffer);

//   temp = video->buffer;
//   video->buffer = outbuf;

//   ret_process_buffer = videoprep->priv->ProcessBuffer(trans, audio, video);
//   if (ret_process_buffer == BufferResult::Buffer_Error) {
//     gst_buffer_unref(outbuf);
//     video->buffer = temp;
//     GST_ERROR_ON_BUS("ProcessBuffer Error", "ProcessBuffer Error");
//     return false;
//   }

//   if (ret_process_buffer == BufferResult::Buffer_Ok) {
//     ret = gst_pad_push(srcpad, outbuf);
//     if (ret != GST_FLOW_OK) {
//       video->buffer = temp;
//       GST_ERROR_ON_BUS("failed to push buffer", "failed to push buffer");
//       return false;
//     }
//   } else if (ret_process_buffer == BufferResult::Buffer_Drop) {
//     gst_buffer_unref(outbuf);
//   }

//   video->buffer = temp;
//   return true;
// }

void videoprep_add_surface_meta(GstBuffer* out_gst_buf, int num_filled_surfaces, int source_id) {
  NvDewarperSurfaceMeta* surface_meta = (NvDewarperSurfaceMeta*)calloc(1, sizeof(NvDewarperSurfaceMeta));

  surface_meta->num_filled_surfaces = num_filled_surfaces;
  surface_meta->source_id = source_id;

  NvDsMeta* meta = NULL;
  meta =
      gst_buffer_add_nvds_meta(out_gst_buf, surface_meta, NULL, videoprep_meta_copy_func, videoprep_meta_release_func);

  meta->meta_type = NVDS_DEWARPER_GST_META;
  meta->gst_to_nvds_meta_transform_func = videoprep_gst_to_nvds_meta_ransform_func;
  meta->gst_to_nvds_meta_release_func = videoprep_gst_nvds_meta_release_func;
}

static cudaError gst_videoprep_do_prep(
    NvDsBatchMeta* batch_meta,
    GstVideoPrep* videoprep,
    NvBufSurface* in_surface,
    NvBufSurface* out_surface) {
  cudaError cudaErr = cudaSuccess;
  gint err = 0;

  err = err;
  GST_LOG_OBJECT(videoprep, "SETTING CUDA DEVICE = %d in videoprep func=%s\n", videoprep->gpu_id, __func__);
  cudaErr = cudaSetDevice(videoprep->gpu_id);
  if (cudaErr != cudaSuccess) {
    printf("\n *** Unable to set device in %s Line %d\n", __func__, __LINE__);
    return cudaErr;
  }

  if (videoprep->output_fmt == GST_VIDEO_FORMAT_NV12 || videoprep->output_fmt == GST_VIDEO_FORMAT_NV21) {
    // RGBA ---> NV12 conversion
    g_print("RGBA to NV12 conversion is not supported in videoprep. Exiting...\n");
    exit(-1);
  } else if (videoprep->output_fmt == GST_VIDEO_FORMAT_RGBA || videoprep->output_fmt == GST_VIDEO_FORMAT_BGRx) {
    cuda_ck(videoprep->priv->GenerateOutput(batch_meta, videoprep, in_surface, out_surface));
  }

  if (videoprep->dump_frames) {
    videoprep->dump_frames--;
  }

  BAIL_IF_FALSE(cudaSuccess == cudaErr, err, (gint)cudaErr);
  return cudaErr;

bail:
  g_print(
      "%s: %s failed at line %d, Error : %d Exiting ...\n", GST_ELEMENT_NAME(videoprep), __func__, __LINE__, cudaErr);
  exit(-1);
  return cudaErr;
}

static GstFlowReturn gst_videoprep_transform(GstBaseTransform* btrans, GstBuffer* inbuf, GstBuffer* outbuf) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(btrans);
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  GstMapInfo outmap = GST_MAP_INFO_INIT;
  NvBufSurface* in_surface = NULL;
  NvBufSurface* out_surface = NULL;
  cudaError cudaErr = cudaSuccess;
  gchar pts_str[64];

  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  assert(batch_meta);

  videoprep->frame_num++;
  GST_DEBUG_OBJECT(videoprep, "%s : Frame=%d InBuf=%p OutBuf=%p\n", __func__, videoprep->frame_num, inbuf, outbuf);

  if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ))
    goto invalid_inbuf;

  if (!gst_buffer_map(outbuf, &outmap, GST_MAP_WRITE))
    goto invalid_outbuf;

  GST_DEBUG_OBJECT(videoprep, "transform");
  if (videoprep->input_feature == MEM_FEATURE_NVMM) {
    in_surface = (NvBufSurface*)inmap.data;
    // TODO:
    if (CHECK_NVDS_MEMORY_AND_GPUID(videoprep, in_surface)) {
      gst_buffer_unmap(inbuf, &inmap);
      gst_buffer_unmap(outbuf, &outmap);
      return GST_FLOW_ERROR;
    }
  }

  if (videoprep->output_feature == MEM_FEATURE_NVMM) {
    out_surface = (NvBufSurface*)outmap.data;
    if (CHECK_NVDS_MEMORY_AND_GPUID(videoprep, out_surface)) {
      gst_buffer_unmap(inbuf, &inmap);
      gst_buffer_unmap(outbuf, &outmap);
      return GST_FLOW_ERROR;
    }
  }

  START_PROFILE;
  videoprep->out_gst_buf = outbuf;
  cudaErr = gst_videoprep_do_prep(batch_meta, videoprep, in_surface, out_surface);
  if (cudaErr != cudaSuccess) {
    GST_ERROR_OBJECT(videoprep, "gst_videoprep_do_prep failed");
    return GST_FLOW_ERROR;
  }
  STOP_PROFILE("********* TOTAL DEWARP AND SCALE TIME *********");

  GST_BUFFER_PTS(outbuf) = GST_BUFFER_PTS(inbuf);

  GST_INFO_OBJECT(
      videoprep,
      " : source_id %d Frame=%d OUT-BUFFER %s",
      videoprep->source_id,
      videoprep->frame_num,
      print_pretty_time(pts_str, sizeof(pts_str), GST_BUFFER_PTS(outbuf)));

  gst_buffer_unmap(inbuf, &inmap);
  gst_buffer_unmap(outbuf, &outmap);

  if (!gst_buffer_copy_into(outbuf, inbuf, (GstBufferCopyFlags)GST_BUFFER_COPY_METADATA, 0, -1)) {
    GST_DEBUG_OBJECT(videoprep, "Buffer metadata copy failed \n");
  }
  return GST_FLOW_OK;

invalid_inbuf: {
  GST_ERROR("input buffer mapinfo failed");
  return GST_FLOW_ERROR;
}

invalid_outbuf: {
  GST_ERROR_OBJECT(videoprep, "output buffer mapinfo failed");
  gst_buffer_unmap(inbuf, &inmap);
  return GST_FLOW_ERROR;
}
}

static GstFlowReturn gst_videoprep_prepare_output_buffer(
    GstBaseTransform* trans,
    GstBuffer* inbuf,
    GstBuffer** outbuf) {
  GstBuffer* gstOutBuf = NULL;
  GstFlowReturn result = GST_FLOW_OK;
  GstVideoPrep* videoprep = GST_VIDEOPREP(trans);

  result = gst_buffer_pool_acquire_buffer(videoprep->pool, &gstOutBuf, NULL);
  GST_DEBUG_OBJECT(videoprep, "%s : Frame=%d Gst-OutBuf=%p\n", __func__, videoprep->frame_num, gstOutBuf);

  if (result != GST_FLOW_OK) {
    GST_ERROR_OBJECT(videoprep, "gst_videoprep_prepare_output_buffer failed");
    return result;
  }

  *outbuf = gstOutBuf;
  return result;
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
  cudaError_t CUerr = cudaSuccess;

  GST_INFO_OBJECT(videoprep, " %s\n", __func__);

  GST_LOG_OBJECT(videoprep, "SETTING CUDA DEVICE = %d in videoprep func=%s\n", videoprep->gpu_id, __func__);
  CUerr = cudaSetDevice(videoprep->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ERROR_OBJECT(videoprep, "cudaSetDevice Failed in %s\n", __func__);
    return FALSE;
  }

  if (videoprep->stream) {
    cuda_ck(cudaStreamDestroy(videoprep->stream));
    videoprep->stream = NULL;
  }

  if (videoprep->pool) {
    gst_buffer_pool_set_active(videoprep->pool, FALSE);
    gst_object_unref(videoprep->pool);
    videoprep->pool = NULL;
  }

  GST_DEBUG_OBJECT(videoprep, "gst_videoprep_stop");

  return TRUE;
}

// static GstStateChangeReturn
// gst_nvdsA2Vtemplate_change_state (GstElement *bscope, GstStateChange transition)
// {
//   if(transition==GST_STATE_CHANGE_NULL_TO_READY) {
//     //GstNvDsA2Vtemplate *scope = GST_NVDSA2VTEMPLATE (bscope);
//     DSCustom_CreateParams params = {0};

//     bool ret;
//     // try {
//     //   scope->algo_factory = new DSCustomLibrary_Factory();
//     //   scope->algo_ctx = scope->algo_factory->CreateCustomAlgoCtx(scope->custom_lib_name, G_OBJECT(bscope));

//     //   if(scope->algo_ctx && scope->vecProp && scope->vecProp && scope->vecProp->size()) {
//     //       GST_INFO_OBJECT(scope, "Setting custom lib properties # %lu", scope->vecProp->size());
//     //       for(std::vector<Property>::iterator it = scope->vecProp->begin(); it != scope->vecProp->end(); ++it) {
//     //           GST_INFO_OBJECT(scope, "Adding Prop: %s : %s", it->key.c_str(), it->value.c_str());
//     //           ret = scope->algo_ctx->SetProperty(*it);
//     //           if (!ret) {
//     //              return GST_STATE_CHANGE_FAILURE;
//     //           }
//     //       }
//     //   }
//     // }
//     // catch (const std::runtime_error& e) {
//     //   GST_ERROR_ON_BUS("Exception occurred", "Runtime error: %s", e.what());
//     //   return GST_STATE_CHANGE_FAILURE;
//     // }
//     // catch (...) {
//     //   GST_ERROR_ON_BUS("Exception occurred", "Exception occurred");
//     //   return GST_STATE_CHANGE_FAILURE;
//     // }
//     params.m_element = bscope;

//     if(!scope->algo_ctx->SetInitParams(&params)) {
//       GST_ERROR_ON_BUS("SetInitParams Error", "SetInitParams Error");
//       return GST_STATE_CHANGE_FAILURE;
//     }
//   }
//   return GST_NVDSA2VTEMPLATE_GET_CLASS(bscope)->parent_change_state_fn(bscope,transition);
// }

/* initialize the videoprep's class */
void gst_videoprep_class_init_base(GstVideoPrepClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseTransformClass* gstbasetransform_class = (GstBaseTransformClass*)klass;

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;

  // Indicates we want to use DS buf api
  g_setenv("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class->set_property = gst_videoprep_set_property;
  gobject_class->get_property = gst_videoprep_get_property;
  gobject_class->finalize = gst_videoprep_finalize;

  // klass->parent_change_state_fn =   gstelement_class->change_state;

  gstbasetransform_class->transform_caps = GST_DEBUG_FUNCPTR(gst_videoprep_transform_caps);
  gstbasetransform_class->fixate_caps = GST_DEBUG_FUNCPTR(gst_videoprep_fixate_caps);
  gstbasetransform_class->accept_caps = GST_DEBUG_FUNCPTR(gst_videoprep_accept_caps);
  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR(gst_videoprep_set_caps);

  gstbasetransform_class->transform = GST_DEBUG_FUNCPTR(gst_videoprep_transform);
  gstbasetransform_class->prepare_output_buffer = GST_DEBUG_FUNCPTR(gst_videoprep_prepare_output_buffer);

  gstbasetransform_class->start = GST_DEBUG_FUNCPTR(gst_videoprep_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR(gst_videoprep_stop);

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
          "num-batch-buffers",
          "Number of Surfaces per output "
          "Buffer",
          "Number of Surfaces per output Buffer",
          0,
          MAX_BUFFERS,
          DEFAULT_NUM_VIDEO_PREPPED_SURFACES,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  g_object_class_install_property(
      gobject_class,
      PROP_CONFIG_FILE,
      g_param_spec_string(
          "config-file",
          "Config File",
          "Config File",
          NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_PLUGIN_TYPE,
      g_param_spec_string(
          "plugin-type",
          "Plugin Type",
          "Plugin Type",
          NULL,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_DEWARP_LIB_VERSION,
      g_param_spec_string(
          "videoprep-lib-version",
          "Library Version",
          "Library Version",
          NULL,
          (GParamFlags)(G_PARAM_READABLE | G_PARAM_STATIC_STRINGS)));
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
  videoprep->pool = NULL;

  videoprep->num_batch_buffers = DEFAULT_NUM_VIDEO_PREPPED_SURFACES;
  videoprep->cuda_mem_type = NVBUF_MEM_DEFAULT;
  videoprep->interpolation_method = NvBufSurfTransformInter_Default;

  // TODO: If CSV is not given then we should not check this
  videoprep->config_file = NULL;
  videoprep->plugin_type = strdup("videoprep");
  videoprep->priv_factory = new VideoPrepLibrary_Factory();

  videoprep->num_output_buffers = DEFAULT_NUM_OUTPUT_BUFFERS;

  videoprep->dump_frames = DEFAULT_DEWARP_DUMP_FRAMES;

  videoprep->output_width = DEFAULT_DEWARP_OUTPUT_WIDTH;
  videoprep->output_height = DEFAULT_DEWARP_OUTPUT_HEIGHT;
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

static void gst_videoprep_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(object);

  switch (prop_id) {
    case PROP_SILENT:
      videoprep->silent = g_value_get_boolean(value);
      break;
    case PROP_GPU_DEVICE_ID:
      videoprep->gpu_id = g_value_get_uint(value);
      break;
    case PROP_SOURCE_ID:
      videoprep->source_id = g_value_get_uint(value);
      break;
    case PROP_NUM_OUTPUT_BUFFERS:
      videoprep->num_output_buffers = g_value_get_uint(value);
      break;
    case PROP_NUM_BATCH_BUFFERS:
      videoprep->num_batch_buffers = g_value_get_uint(value);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      videoprep->cuda_mem_type = static_cast<NvBufSurfaceMemType>(g_value_get_enum(value));
      break;
    case PROP_INTERPOLATION_METHOD:
      videoprep->interpolation_method = static_cast<NvBufSurfTransform_Inter>(g_value_get_enum(value));
      break;
    case PROP_CONFIG_FILE:
      if (videoprep->config_file)
        g_free(videoprep->config_file);
      videoprep->config_file = (gchar*)g_value_dup_string(value);
      if (videoprep_parse_config_file(videoprep, videoprep->config_file) != TRUE) {
        g_print("%s: Failed to parse config file %s\n", GST_ELEMENT_NAME(videoprep), videoprep->config_file);
        abort();
      }
      break;
    case PROP_PLUGIN_TYPE:
      if (videoprep->plugin_type)
        g_free(videoprep->plugin_type);
      videoprep->plugin_type = (gchar*)g_value_dup_string(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

static void gst_videoprep_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  GstVideoPrep* videoprep = GST_VIDEOPREP(object);

  switch (prop_id) {
    case PROP_SILENT:
      g_value_set_boolean(value, videoprep->silent);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint(value, videoprep->gpu_id);
      break;
    case PROP_SOURCE_ID:
      g_value_set_uint(value, videoprep->source_id);
      break;
    case PROP_NUM_OUTPUT_BUFFERS:
      g_value_set_uint(value, videoprep->num_output_buffers);
      break;
    case PROP_NUM_BATCH_BUFFERS:
      g_value_set_uint(value, videoprep->num_batch_buffers);
      break;
    case PROP_CONFIG_FILE:
      g_value_set_string(value, videoprep->config_file);
      break;
    case PROP_PLUGIN_TYPE:
      g_value_set_string(value, videoprep->plugin_type);
      break;
    case PROP_DEWARP_LIB_VERSION:
      g_value_set_static_string(value, VIDEOPREP_LIB_VERSION);
      break;
    case PROP_NVBUF_MEMORY_TYPE:
      g_value_set_enum(value, videoprep->cuda_mem_type);
      break;
    case PROP_INTERPOLATION_METHOD:
      g_value_set_enum(value, videoprep->interpolation_method);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

} // namespace videoprep
} // namespace hm
