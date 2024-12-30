/*
 * SPDX-FileCopyrightText: Copyright (c) 2017-2024 NVIDIA CORPORATION &
 * AFFILIATES. All rights reserved. SPDX-License-Identifier:
 * LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */
#include "gstdsreplaceframe.h"

// #include "gst-nvquery.h"
#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
// #include "nvbufsurftransform.h"

#include <gst/gst.h>
#include <gst/video/gstvideofilter.h>
#include <gst/video/gstvideosink.h>
#include <gst/video/video.h>
#include <nvbufsurface.h>
#include <nvbufsurftransform.h>
#include <nvdsmeta.h>

#include <string.h>
#include <sys/time.h>
#include <cassert>
#include <string>

#if 0

namespace {
GST_DEBUG_CATEGORY_STATIC(gst_dsreplaceframe_debug);
#define GST_CAT_DEFAULT gst_dsreplaceframe_debug
#define USE_EGLIMAGE 1
/* enable to write transformed cvmat to files */
/* #define DSREPLACEFRAME_DEBUG */
static GQuark _dsmeta_quark = 0;

/* Enum to identify properties */
enum {
  PROP_0,
  PROP_UNIQUE_ID,
  PROP_GPU_DEVICE_ID,
  PROP_DETECTION_MASK_FILE,
};

#define CHECK_NVDS_MEMORY_AND_GPUID(object, surface)                                                       \
  ({                                                                                                       \
    int _errtype = 0;                                                                                      \
    do {                                                                                                   \
      if ((surface->memType == NVBUF_MEM_DEFAULT || surface->memType == NVBUF_MEM_CUDA_DEVICE) &&          \
          (surface->gpuId != object->gpu_id)) {                                                            \
        GST_ELEMENT_ERROR(                                                                                 \
            object,                                                                                        \
            RESOURCE,                                                                                      \
            FAILED,                                                                                        \
            ("Input surface gpu-id doesnt match with configured gpu-id for element,"                       \
             " please allocate input using unified memory, or use same gpu-ids"),                          \
            ("surface-gpu-id=%d,%s-gpu-id=%d", surface->gpuId, GST_ELEMENT_NAME(object), object->gpu_id)); \
        _errtype = 1;                                                                                      \
      }                                                                                                    \
    } while (0);                                                                                           \
    _errtype;                                                                                              \
  })

/* Default values for properties */
#define DEFAULT_UNIQUE_ID 15
#define DEFAULT_PROCESSING_WIDTH 640
#define DEFAULT_PROCESSING_HEIGHT 480
#define DEFAULT_GPU_ID 0
#define DEFAULT_BATCH_SIZE 1

#define RGB_BYTES_PER_PIXEL 3
#define RGBA_BYTES_PER_PIXEL 4
#define Y_BYTES_PER_PIXEL 1
#define UV_BYTES_PER_PIXEL 2

#define MIN_INPUT_OBJECT_WIDTH 1
#define MIN_INPUT_OBJECT_HEIGHT 1

#define CHECK_NPP_STATUS(npp_status, error_str)                                                         \
  do {                                                                                                  \
    if ((npp_status) != NPP_SUCCESS) {                                                                  \
      g_print("Error: %s in %s at line %d: NPP Error %d\n", error_str, __FILE__, __LINE__, npp_status); \
      goto error;                                                                                       \
    }                                                                                                   \
  } while (0)

#define CHECK_CUDA_STATUS(cuda_status, error_str)                                                                 \
  do {                                                                                                            \
    if ((cuda_status) != cudaSuccess) {                                                                           \
      g_print("Error: %s in %s at line %d (%s)\n", error_str, __FILE__, __LINE__, cudaGetErrorName(cuda_status)); \
      goto error;                                                                                                 \
    }                                                                                                             \
  } while (0)

#define STRSIZE(str$) (sizeof(str$) / sizeof(str$[0]))

/* By default NVIDIA Hardware allocated memory flows through the pipeline. We
 * will be processing on this type of memory only. */
#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"
static GstStaticPadTemplate gst_dsreplaceframe_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_dsreplaceframe_src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_dsreplaceframe_parent_class parent_class
G_DEFINE_TYPE(GstDsReplaceFrame, gst_dsreplaceframe, GST_TYPE_BASE_TRANSFORM);

static void gst_dsreplaceframe_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gst_dsreplaceframe_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

static gboolean gst_dsreplaceframe_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps);
static gboolean gst_dsreplaceframe_start(GstBaseTransform* btrans);
static gboolean gst_dsreplaceframe_stop(GstBaseTransform* btrans);

static GstFlowReturn gst_dsreplaceframe_transform_ip(GstBaseTransform* btrans, GstBuffer* inbuf);

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void gst_dsreplaceframe_class_init(GstDsReplaceFrameClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseTransformClass* gstbasetransform_class;
  /* Indicates we want to use DS buf api */
  g_setenv("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;
  gstbasetransform_class = (GstBaseTransformClass*)klass;

  /* Overide base class functions */
  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_dsreplaceframe_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_dsreplaceframe_get_property);

  // gstbasetransform_class->submit_input_buffer =
  //     GST_DEBUG_FUNCPTR(gst_ds_example_submit_input_buffer);
  // gstbasetransform_class->generate_output =
  //     GST_DEBUG_FUNCPTR(gst_ds_example_generate_output);

  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR(gst_dsreplaceframe_set_caps);
  gstbasetransform_class->start = GST_DEBUG_FUNCPTR(gst_dsreplaceframe_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR(gst_dsreplaceframe_stop);

  //   /** NOTE: Initializing state to nullptr is essential. */
  // NvDsBatchMeta *batch_meta = nullptr;
  // GstMapInfo inmap;

  // batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);

  gstbasetransform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_dsreplaceframe_transform_ip);

  /* Install properties */
  g_object_class_install_property(
      gobject_class,
      PROP_UNIQUE_ID,
      g_param_spec_uint(
          "unique-id",
          "Unique ID",
          "Unique ID for the element. Can be used to identify output of the"
          " element",
          0,
          G_MAXUINT,
          DEFAULT_UNIQUE_ID,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_DETECTION_MASK_FILE,
      g_param_spec_string(
          "detection-mask",
          "Detection Mask",
          "Restrict detections to position mask",
          /*default_value=*/"",
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_GPU_DEVICE_ID,
      g_param_spec_uint(
          "gpu-id",
          "Set GPU Device ID",
          "Set GPU Device ID",
          0,
          G_MAXUINT,
          0,
          GParamFlags(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

  /* Set sink and src pad capabilities */
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_dsreplaceframe_src_template));
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_dsreplaceframe_sink_template));

  /* Set metadata describing the element */
  gst_element_class_set_details_simple(
      gstelement_class,
      "DsReplaceFrame plugin",
      "DsReplaceFrame Plugin",
      "Process a 3rdparty example algorithm on objects / full frame",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");
}

static void gst_dsreplaceframe_init(GstDsReplaceFrame* dsreplaceframe) {
  // std::cerr << "element ASSERTING" << std::endl;
  // assert(false);
  GstBaseTransform* btrans = GST_BASE_TRANSFORM(dsreplaceframe);

  /* We will not be generating a new buffer. Just adding / updating
   * metadata. */
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(btrans), TRUE);
  /* We do not want to change the input caps. Set to passthrough. transform_ip
   * is still called. */
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(btrans), TRUE);

  /* Initialize all property variables to default values */
  dsreplaceframe->unique_id = DEFAULT_UNIQUE_ID;
  dsreplaceframe->gpu_id = DEFAULT_GPU_ID;

  /* This quark is required to identify NvDsMeta when iterating through
   * the buffer metadatas */
  if (!_dsmeta_quark)
    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void gst_dsreplaceframe_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  GstDsReplaceFrame* dsreplaceframe = GST_DSREPLACEFRAME(object);
  //_GstDsReplaceFrame* dsreplaceframe = dynamic_cast<_GstDsReplaceFrame*>(object);
  assert(dsreplaceframe);
  switch (prop_id) {
    case PROP_UNIQUE_ID:
      dsreplaceframe->unique_id = g_value_get_uint(value);
      break;
    case PROP_GPU_DEVICE_ID:
      dsreplaceframe->gpu_id = g_value_get_uint(value);
      break;
    case PROP_DETECTION_MASK_FILE:
    {
      const char *str = g_value_get_string(value);
      if (str && *str) {
          strncpy(dsreplaceframe->detection_mask_file, str, STRSIZE(dsreplaceframe->detection_mask_file) - 1);
      } else {
        dsreplaceframe->detection_mask_file[0] = '\0';
      }
    }
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void gst_dsreplaceframe_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  GstDsReplaceFrame* dsreplaceframe = GST_DSREPLACEFRAME(object);
  assert(dsreplaceframe);
  switch (prop_id) {
    case PROP_UNIQUE_ID:
      g_value_set_uint(value, dsreplaceframe->unique_id);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint(value, dsreplaceframe->gpu_id);
      break;
    case PROP_DETECTION_MASK_FILE:
      g_value_set_string(value, dsreplaceframe->detection_mask_file);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/**
 * Initialize all resources and start the output thread
 */
static gboolean gst_dsreplaceframe_start(GstBaseTransform* btrans) {
  GstDsReplaceFrame* dsreplaceframe = GST_DSREPLACEFRAME(btrans);

  DsReplaceFrameInitParams init_params = {
      .detection_mask_file = dsreplaceframe->detection_mask_file};

  /* Algorithm specific initializations and resource allocation. */
  dsreplaceframe->dsreplaceframelib_ctx = DsReplaceFrameCtxInit(&init_params);

  GST_DEBUG_OBJECT(dsreplaceframe, "ctx lib %p \n", dsreplaceframe->dsreplaceframelib_ctx);

  CHECK_CUDA_STATUS(cudaSetDevice(dsreplaceframe->gpu_id), "Unable to set cuda device");

  /* Create host memory for storing converted/scaled interleaved RGB data */
  return TRUE;
error:
  return FALSE;
}

/**
 * Stop the output thread and free up all the resources
 */
static gboolean gst_dsreplaceframe_stop(GstBaseTransform* btrans) {
  GstDsReplaceFrame* dsreplaceframe = GST_DSREPLACEFRAME(btrans);

  GST_DEBUG_OBJECT(dsreplaceframe, "deleted CV Mat \n");

  /* Deinit the algorithm library */
  DsReplaceFrameCtxDeinit(dsreplaceframe->dsreplaceframelib_ctx);
  dsreplaceframe->dsreplaceframelib_ctx = NULL;

  GST_DEBUG_OBJECT(dsreplaceframe, "ctx lib released \n");

  return TRUE;
}

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean gst_dsreplaceframe_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps) {
  GstDsReplaceFrame* dsreplaceframe = GST_DSREPLACEFRAME(btrans);
  /* Save the input video information, since this will be required later. */
  gst_video_info_from_caps(&dsreplaceframe->video_info, incaps);

  return TRUE;
}

/**
 * Called when element recieves an input buffer from upstream element.
 */
static GstFlowReturn gst_dsreplaceframe_transform_ip(GstBaseTransform* btrans, GstBuffer* inbuf) {
  GstDsReplaceFrame* dsreplaceframe = GST_DSREPLACEFRAME(btrans);
  GstMapInfo in_map_info;
  GstFlowReturn flow_ret = GST_FLOW_ERROR;

  NvBufSurface* surface = NULL;
  NvDsBatchMeta* batch_meta = NULL;
  NvDsFrameMeta* frame_meta = NULL;
  NvDsMetaList* l_frame = NULL;

  dsreplaceframe->frame_num++;
  CHECK_CUDA_STATUS(cudaSetDevice(dsreplaceframe->gpu_id), "Unable to set cuda device");

  memset(&in_map_info, 0, sizeof(in_map_info));
  if (!gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
    g_print("Error: Failed to map gst buffer\n");
    goto error;
  }

  nvds_set_input_system_timestamp(inbuf, GST_ELEMENT_NAME(dsreplaceframe));
  surface = (NvBufSurface*)in_map_info.data;
  GST_DEBUG_OBJECT(dsreplaceframe, "Processing Frame %" G_GUINT64_FORMAT " Surface %p\n", dsreplaceframe->frame_num, surface);

  if (CHECK_NVDS_MEMORY_AND_GPUID(dsreplaceframe, surface))
    goto error;

  batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  if (batch_meta == nullptr) {
    GST_ELEMENT_ERROR(dsreplaceframe, STREAM, FAILED, ("NvDsBatchMeta not found for input buffer."), (NULL));
    return GST_FLOW_ERROR;
  }

  /* Using object crops as input to the algorithm. The objects are detected by
   * the primary detector */

  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    frame_meta = (NvDsFrameMeta*)(l_frame->data);

    DsReplaceFrameProcessFrame(frame_meta, dsreplaceframe->dsreplaceframelib_ctx);
  }
  flow_ret = GST_FLOW_OK;

error:

  nvds_set_output_system_timestamp(inbuf, GST_ELEMENT_NAME(dsreplaceframe));
  gst_buffer_unmap(inbuf, &in_map_info);
  return flow_ret;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean dsreplaceframe_plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_dsreplaceframe_debug, "dsreplaceframe", 0, "dsreplaceframe plugin");

  return gst_element_register(plugin, "dsreplaceframe", GST_RANK_PRIMARY, GST_TYPE_DSREPLACEFRAME);
}
} // namespace
#else

// Boilerplate GStreamer plugin definitions
//GST_DEBUG_CATEGORY_STATIC(custom_plugin_debug);
#define GST_CAT_DEFAULT custom_plugin_debug

#define GST_TYPE_CUSTOM_PLUGIN (gst_custom_plugin_get_type())
G_DECLARE_FINAL_TYPE(GstCustomPlugin, gst_custom_plugin, GST, CUSTOM_PLUGIN, GstElement)

struct _GstCustomPlugin {
  GstElement parent;

  GstPad* sink_pad_1; // For metadata sink
  GstPad* sink_pad_2; // For video frame sink
  GstPad* src_pad; // For source

  GstBuffer* sink_1_buffer; // Buffer for sink 1
  GstBuffer* sink_2_buffer; // Buffer for sink 2
};

G_DEFINE_TYPE(GstCustomPlugin, gst_custom_plugin, GST_TYPE_ELEMENT);

static GstFlowReturn gst_custom_plugin_chain_sink_1(GstPad* pad, GstObject* parent, GstBuffer* buffer) {
  GstCustomPlugin* plugin = GST_CUSTOM_PLUGIN(parent);

  // Store the metadata buffer from sink 1
  if (plugin->sink_1_buffer) {
    gst_buffer_unref(plugin->sink_1_buffer);
  }
  plugin->sink_1_buffer = gst_buffer_ref(buffer);

  // Attempt to join with sink 2 buffer if available
  if (plugin->sink_2_buffer) {
    GstBuffer* output_buffer = gst_buffer_ref(plugin->sink_2_buffer);

    // Copy metadata from sink 1 buffer to sink 2 buffer
    GstMeta* meta;
    gpointer state = NULL;
    while ((meta = gst_buffer_iterate_meta(plugin->sink_1_buffer, &state))) {
      // ???
      gst_buffer_add_meta(output_buffer, meta->info, nullptr);
    }

    // Push the combined buffer downstream
    gst_pad_push(plugin->src_pad, output_buffer);

    gst_buffer_unref(plugin->sink_2_buffer);
    plugin->sink_2_buffer = NULL;
  }

  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

static GstFlowReturn gst_custom_plugin_chain_sink_2(GstPad* pad, GstObject* parent, GstBuffer* buffer) {
  GstCustomPlugin* plugin = GST_CUSTOM_PLUGIN(parent);

  // Store the video frame buffer from sink 2
  if (plugin->sink_2_buffer) {
    gst_buffer_unref(plugin->sink_2_buffer);
  }
  plugin->sink_2_buffer = gst_buffer_ref(buffer);

  // Attempt to join with sink 1 buffer if available
  if (plugin->sink_1_buffer) {
    GstBuffer* output_buffer = gst_buffer_ref(buffer);

    // Copy metadata from sink 1 buffer to sink 2 buffer
    GstMeta* meta;
    gpointer state = NULL;
    while ((meta = gst_buffer_iterate_meta(plugin->sink_1_buffer, &state))) {
      gst_buffer_add_meta(output_buffer, meta->info, nullptr);
      // gst_buffer_add_meta(output_buffer, meta);
    }

    // Push the combined buffer downstream
    gst_pad_push(plugin->src_pad, output_buffer);

    gst_buffer_unref(plugin->sink_1_buffer);
    plugin->sink_1_buffer = NULL;
  }

  gst_buffer_unref(buffer);
  return GST_FLOW_OK;
}

#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"
static GstStaticPadTemplate gst_dsreplaceframe_sink_template_1 = GST_STATIC_PAD_TEMPLATE(
    "sink_1",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_dsreplaceframe_sink_template_2 = GST_STATIC_PAD_TEMPLATE(
    "sink_2",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_dsreplaceframe_src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static void gst_custom_plugin_class_init(GstCustomPluginClass* klass) {
  GstElementClass* element_class = GST_ELEMENT_CLASS(klass);

  gst_element_class_set_static_metadata(
      element_class,
      "Custom Plugin",
      "Filter/Video",
      "Custom plugin with two sinks and one source",
      "Your Name <your.email@example.com>");

  // gst_element_class_add_pad_template(
  //     element_class,
  //     gst_static_pad_template_get(
  //         &gst_static_pad_template_factory("sink_1", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_new_any())));

  // gst_element_class_add_pad_template(
  //     element_class,
  //     gst_static_pad_template_get(
  //         &gst_static_pad_template_factory("sink_2", GST_PAD_SINK, GST_PAD_ALWAYS, gst_caps_new_any())));

  // gst_element_class_add_pad_template(
  //     element_class,
  //     gst_static_pad_template_get(
  //         &gst_static_pad_template_factory("src", GST_PAD_SRC, GST_PAD_ALWAYS, gst_caps_new_any())));

  /* Set sink and src pad capabilities */
  gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_dsreplaceframe_src_template));
  gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_dsreplaceframe_sink_template_1));
  gst_element_class_add_pad_template(element_class, gst_static_pad_template_get(&gst_dsreplaceframe_sink_template_2));
}

static void gst_custom_plugin_init(GstCustomPlugin* plugin) {
  plugin->sink_pad_1 = gst_pad_new_from_static_template(&gst_dsreplaceframe_sink_template_1, "sink_1");
  gst_pad_set_chain_function(plugin->sink_pad_1, gst_custom_plugin_chain_sink_1);
  gst_element_add_pad(GST_ELEMENT(plugin), plugin->sink_pad_1);

  plugin->sink_pad_2 = gst_pad_new_from_static_template(&gst_dsreplaceframe_sink_template_2, "sink_2");
  gst_pad_set_chain_function(plugin->sink_pad_2, gst_custom_plugin_chain_sink_2);
  gst_element_add_pad(GST_ELEMENT(plugin), plugin->sink_pad_2);

  plugin->src_pad = gst_pad_new_from_static_template(&gst_dsreplaceframe_src_template, "src");
  gst_element_add_pad(GST_ELEMENT(plugin), plugin->src_pad);

  plugin->sink_1_buffer = NULL;
  plugin->sink_2_buffer = NULL;
}

static gboolean dsreplaceframe_plugin_init(GstPlugin* plugin) {
  return gst_element_register(plugin, "dsreplaceframeq", GST_RANK_NONE, GST_TYPE_CUSTOM_PLUGIN);
}

#endif
GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvdsgst_dsreplaceframe,
    DESCRIPTION,
    dsreplaceframe_plugin_init,
    "7.1",
    LICENSE,
    BINARY_PACKAGE,
    URL)
