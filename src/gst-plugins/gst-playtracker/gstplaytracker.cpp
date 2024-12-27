/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2024 NVIDIA CORPORATION &
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

/**
 * There are two threads in the optimized code. input thread and Processing
 * thread. The pre-procesing as required by the algorithm like scaling and color
 * conversion of data is done in input thread. This is done using
 * NvBufSurfTransform's batch conversion APIs to improve performance. The
 * processing of data using custom algorithm and parsing the output and metadata
 * attachment is done in separate processing thread.
 *
 * There are two queues used for buffering and transferring data between thread:
 * Process_queue and buf_queue Process_queue is used to send filled batched data
 * to process thread and buf_queue is used to get return empty processed buffers
 * from process thread to input thread.  Two buffers are used in a ping pong
 * manner between the two threads for parallel processing.
 */

#include <string.h>
// #include <fstream>
// #include <iostream>
// #include <ostream>
// #include <sstream>
#include <string>

#include "gstplaytracker.h"
#include "utils.h"

#include <sys/time.h>
// #include <condition_variable>
// #include <mutex>
#include <memory>
// #include <thread>
#include <cassert>

GST_DEBUG_CATEGORY_STATIC(gst_playtracker_debug);
#define GST_CAT_DEFAULT gst_playtracker_debug
#define USE_EGLIMAGE 1

static GQuark _dsmeta_quark = 0;

/* Enum to identify properties */
enum {
  PROP_0,
  PROP_UNIQUE_ID,
  PROP_PROCESSING_WIDTH,
  PROP_PROCESSING_HEIGHT,
  PROP_DRAW,
  PROP_BATCH_SIZE,
  PROP_GPU_DEVICE_ID,
  PROP_PLAY_TRACKER_CONFIG_FILE,
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
#define DEFAULT_PROCESS_FULL_FRAME FALSE
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

/* By default NVIDIA Hardware allocated memory flows through the pipeline. We
 * will be processing on this type of memory only. */
#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"
static GstStaticPadTemplate gst_playtracker_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_playtracker_src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_playtracker_parent_class parent_class
G_DEFINE_TYPE(GstDsPlayTracker, gst_playtracker, GST_TYPE_BASE_TRANSFORM);

static void gst_playtracker_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gst_playtracker_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

static gboolean gst_playtracker_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps);
static gboolean gst_playtracker_start(GstBaseTransform* btrans);
static gboolean gst_playtracker_stop(GstBaseTransform* btrans);

static GstFlowReturn gst_playtracker_submit_input_buffer(GstBaseTransform* btrans, gboolean discont, GstBuffer* inbuf);
static GstFlowReturn gst_playtracker_generate_output(GstBaseTransform* btrans, GstBuffer** outbuf);

// static void attach_metadata_full_frame(
//     GstDsPlayTracker* playtracker,
//     NvDsFrameMeta* frame_meta,
//     gdouble scale_ratio,
//     DsPlayTrackerOutput* output,
//     guint batch_id);
// static void attach_metadata_object(
//     GstDsPlayTracker* playtracker,
//     NvDsObjectMeta* obj_meta,
//     DsPlayTrackerOutput* output);

static gpointer gst_playtracker_output_loop(gpointer data);

static gboolean gst_playtracker_sink_event(GstBaseTransform* trans, GstEvent* event) {
  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_CAPS: {
      GstCaps* caps;
      gst_event_parse_caps(event, &caps);
      g_print("Received CAPS event: %s\n", gst_caps_to_string(caps));
      break;
    }
    default:
      break;
  }
  // return gst_pad_event_default(pad, parent, event);  return true;
  return GST_BASE_TRANSFORM_CLASS(parent_class)->sink_event(trans, event);
}

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void gst_playtracker_class_init(GstDsPlayTrackerClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseTransformClass* gstbasetransform_class;

  // Indicates we want to use DS buf api
  g_setenv("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;
  gstbasetransform_class = (GstBaseTransformClass*)klass;

  /* Overide base class functions */
  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_playtracker_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_playtracker_get_property);

  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR(gst_playtracker_set_caps);
  gstbasetransform_class->start = GST_DEBUG_FUNCPTR(gst_playtracker_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR(gst_playtracker_stop);

  gstbasetransform_class->sink_event = GST_DEBUG_FUNCPTR(gst_playtracker_sink_event);

  gstbasetransform_class->submit_input_buffer = GST_DEBUG_FUNCPTR(gst_playtracker_submit_input_buffer);
  gstbasetransform_class->generate_output = GST_DEBUG_FUNCPTR(gst_playtracker_generate_output);

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
      PROP_PROCESSING_WIDTH,
      g_param_spec_int(
          "processing-width",
          "Processing Width",
          "Width of the input buffer to algorithm",
          1,
          G_MAXINT,
          DEFAULT_PROCESSING_WIDTH,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_PROCESSING_HEIGHT,
      g_param_spec_int(
          "processing-height",
          "Processing Height",
          "Height of the input buffer to algorithm",
          1,
          G_MAXINT,
          DEFAULT_PROCESSING_HEIGHT,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_DRAW,
      g_param_spec_boolean(
          "draw",
          "Draw tracking boxes",
          "Draw stuff",
          false,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_PLAY_TRACKER_CONFIG_FILE,
      g_param_spec_string(
          "config-file",
          "Play Tracker configuration file",
          "Config of the play tracker (not the plugin)",
          /*default_value=*/"",
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_BATCH_SIZE,
      g_param_spec_uint(
          "batch-size",
          "Batch Size",
          "Maximum batch size for processing",
          1,
          NVDSPLAYTRACKER_MAX_BATCH_SIZE,
          DEFAULT_BATCH_SIZE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));

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
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_playtracker_src_template));
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_playtracker_sink_template));

  /* Set metadata describing the element */
  gst_element_class_set_details_simple(
      gstelement_class,
      "DsPlayTracker plugin",
      "DsPlayTracker Plugin",
      "Process a 3rdparty example algorithm on objects / full frame",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");
}

static void gst_playtracker_init(GstDsPlayTracker* playtracker) {
  GstBaseTransform* btrans = GST_BASE_TRANSFORM(playtracker);

  /* We will not be generating a new buffer. Just adding / updating
   * metadata. */
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(btrans), TRUE);
  /* We do not want to change the input caps. Set to passthrough. transform_ip
   * is still called. */
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(btrans), TRUE);

  /* Initialize all property variables to default values */
  playtracker->unique_id = DEFAULT_UNIQUE_ID;
  playtracker->processing_width = DEFAULT_PROCESSING_WIDTH;
  playtracker->processing_height = DEFAULT_PROCESSING_HEIGHT;
  playtracker->gpu_id = DEFAULT_GPU_ID;
  playtracker->max_batch_size = DEFAULT_BATCH_SIZE;
  /* This quark is required to identify NvDsMeta when iterating through
   * the buffer metadatas */
  if (!_dsmeta_quark)
    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void gst_playtracker_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(object);
  switch (prop_id) {
    case PROP_UNIQUE_ID:
      playtracker->unique_id = g_value_get_uint(value);
      break;
    case PROP_PROCESSING_WIDTH:
      playtracker->processing_width = g_value_get_int(value);
      break;
    case PROP_PROCESSING_HEIGHT:
      playtracker->processing_height = g_value_get_int(value);
      break;
    case PROP_GPU_DEVICE_ID:
      playtracker->gpu_id = g_value_get_uint(value);
      break;
    case PROP_BATCH_SIZE:
      playtracker->max_batch_size = g_value_get_uint(value);
      break;
    case PROP_DRAW:
      playtracker->draw = g_value_get_boolean(value);
      break;
    case PROP_PLAY_TRACKER_CONFIG_FILE: {
      const char* str = g_value_get_string(value);
      if (str && *str) {
        strncpy(playtracker->play_tracker_config_file, str, STRSIZE(playtracker->play_tracker_config_file) - 1);
      } else {
        playtracker->play_tracker_config_file[0] = '\0';
      }
    } break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void gst_playtracker_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(object);

  switch (prop_id) {
    case PROP_UNIQUE_ID:
      g_value_set_uint(value, playtracker->unique_id);
      break;
    case PROP_PROCESSING_WIDTH:
      g_value_set_int(value, playtracker->processing_width);
      break;
    case PROP_PROCESSING_HEIGHT:
      g_value_set_int(value, playtracker->processing_height);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint(value, playtracker->gpu_id);
      break;
    case PROP_BATCH_SIZE:
      g_value_set_uint(value, playtracker->max_batch_size);
      break;
    case PROP_DRAW:
      g_value_set_boolean(value, playtracker->draw);
      break;
    case PROP_PLAY_TRACKER_CONFIG_FILE:
      g_value_set_string(value, playtracker->play_tracker_config_file);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/**
 * Initialize all resources and start the process thread
 */
static gboolean gst_playtracker_start(GstBaseTransform* btrans) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
  std::string nvtx_str;
  NvBufSurface* inter_buf;
  NvBufSurfaceCreateParams create_params = {0};
  DsPlayTrackerInitParams init_params = {
      .processingWidth = playtracker->processing_width,
      .processingHeight = playtracker->processing_height,
      .play_tracker_config_file = playtracker->play_tracker_config_file,
      .draw = !!playtracker->draw,
  };

  /* Algorithm specific initializations and resource allocation. */
  playtracker->playtrackerlib_ctx = DsPlayTrackerCtxInit(&init_params);

  GST_DEBUG_OBJECT(playtracker, "ctx lib %p \n", playtracker->playtrackerlib_ctx);

  nvtx_str = "GstNvDsPlayTracker: UID=" + std::to_string(playtracker->unique_id);
  auto nvtx_deleter = [](nvtxDomainHandle_t d) { nvtxDomainDestroy(d); };
  std::unique_ptr<nvtxDomainRegistration, decltype(nvtx_deleter)> nvtx_domain_ptr(
      nvtxDomainCreate(nvtx_str.c_str()), nvtx_deleter);

  CHECK_CUDA_STATUS(cudaSetDevice(playtracker->gpu_id), "Unable to set cuda device");

  CHECK_CUDA_STATUS(cudaStreamCreate(&playtracker->cuda_stream), "Could not create cuda stream");

  /* An intermediate buffer for NV12/RGBA to BGR conversion  will be
   * required. Can be skipped if custom algorithm can work directly on
   * NV12/RGBA. */
  create_params.gpuId = playtracker->gpu_id;
  create_params.width = playtracker->processing_width;
  create_params.height = playtracker->processing_height;
  create_params.size = 0;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;
#ifdef __aarch64__
  create_params.memType = NVBUF_MEM_DEFAULT;
#else
  create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
#endif

  /* Create process queue and cvmat queue to transfer data between threads.
   * We will be using this queue to maintain the list of frames/objects
   * currently given to the algorithm for processing. */
  playtracker->process_queue = g_queue_new();
  playtracker->buf_queue = g_queue_new();

  for (int i = 0; i < 2; i++) {
    if (NvBufSurfaceCreate(&inter_buf, playtracker->max_batch_size, &create_params) != 0) {
      GST_ERROR("Error: Could not allocate internal buffer for playtracker");
      goto error;
    }

    g_queue_push_tail(playtracker->buf_queue, inter_buf);
  }

  /* Set the NvBufSurfTransform config parameters. */
  playtracker->transform_config_params.compute_mode = NvBufSurfTransformCompute_Default;
  playtracker->transform_config_params.gpu_id = playtracker->gpu_id;

  /* Create the intermediate NvBufSurface structure for holding an array of
   * input NvBufSurfaceParams for batched transforms. */
  playtracker->batch_insurf.surfaceList = new NvBufSurfaceParams[playtracker->max_batch_size];
  playtracker->batch_insurf.batchSize = playtracker->max_batch_size;
  playtracker->batch_insurf.gpuId = playtracker->gpu_id;

  /* Set up the NvBufSurfTransformParams structure for batched transforms. */
  playtracker->transform_params.src_rect = new NvBufSurfTransformRect[playtracker->max_batch_size];
  playtracker->transform_params.dst_rect = new NvBufSurfTransformRect[playtracker->max_batch_size];
  playtracker->transform_params.transform_flag =
      NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST;
  playtracker->transform_params.transform_flip = NvBufSurfTransform_None;
  playtracker->transform_params.transform_filter = NvBufSurfTransformInter_Default;

  /* Start a thread which will pop output from the algorithm, form NvDsMeta and
   * push buffers to the next element. */
  playtracker->process_thread = g_thread_new("playtracker-process-thread", gst_playtracker_output_loop, playtracker);

  playtracker->nvtx_domain = nvtx_domain_ptr.release();

  return TRUE;
error:

  delete[] playtracker->transform_params.src_rect;
  delete[] playtracker->transform_params.dst_rect;
  delete[] playtracker->batch_insurf.surfaceList;

  if (playtracker->cuda_stream) {
    cudaStreamDestroy(playtracker->cuda_stream);
    playtracker->cuda_stream = NULL;
  }
  if (playtracker->playtrackerlib_ctx)
    DsPlayTrackerCtxDeinit(playtracker->playtrackerlib_ctx);
  return FALSE;
}

/**
 * Stop the process thread and free up all the resources
 */
static gboolean gst_playtracker_stop(GstBaseTransform* btrans) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);

  NvBufSurface* inter_buf;

  g_mutex_lock(&playtracker->process_lock);

  /* Wait till all the items in the queue are handled. */
  while (!g_queue_is_empty(playtracker->process_queue)) {
    g_cond_wait(&playtracker->process_cond, &playtracker->process_lock);
  }

  while (!g_queue_is_empty(playtracker->buf_queue)) {
    inter_buf = (NvBufSurface*)g_queue_pop_head(playtracker->buf_queue);
    if (inter_buf)
      NvBufSurfaceDestroy(inter_buf);
    inter_buf = NULL;
  }
  playtracker->stop = TRUE;

  g_cond_broadcast(&playtracker->process_cond);
  g_mutex_unlock(&playtracker->process_lock);

  g_thread_join(playtracker->process_thread);

  if (playtracker->cuda_stream)
    cudaStreamDestroy(playtracker->cuda_stream);
  playtracker->cuda_stream = NULL;

  delete[] playtracker->transform_params.src_rect;
  delete[] playtracker->transform_params.dst_rect;
  delete[] playtracker->batch_insurf.surfaceList;

  // Deinit the algorithm library
  DsPlayTrackerCtxDeinit(playtracker->playtrackerlib_ctx);
  playtracker->playtrackerlib_ctx = NULL;

  GST_DEBUG_OBJECT(playtracker, "ctx lib released \n");

  g_queue_free(playtracker->process_queue);

  g_queue_free(playtracker->buf_queue);

  return TRUE;
}

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean gst_playtracker_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
  /* Save the input video information, since this will be required later. */
  gst_video_info_from_caps(&playtracker->video_info, incaps);

  CHECK_CUDA_STATUS(cudaSetDevice(playtracker->gpu_id), "Unable to set cuda device");

  return TRUE;

error:
  return FALSE;
}

/**
 * Scale the entire frame to the processing resolution maintaining aspect ratio.
 * Or crop and scale objects to the processing resolution maintaining the aspect
 * ratio and fills data for batched conversation */
static GstFlowReturn scale_and_fill_data(
    GstDsPlayTracker* playtracker,
    NvBufSurfaceParams* src_frame,
    NvOSD_RectParams* crop_rect_params,
    gdouble& ratio,
    gint input_width,
    gint input_height) {
  gint src_left = GST_ROUND_UP_2((unsigned int)crop_rect_params->left);
  gint src_top = GST_ROUND_UP_2((unsigned int)crop_rect_params->top);
  gint src_width = GST_ROUND_DOWN_2((unsigned int)crop_rect_params->width);
  gint src_height = GST_ROUND_DOWN_2((unsigned int)crop_rect_params->height);

  // Maintain aspect ratio
  double hdest = playtracker->processing_width * src_height / (double)src_width;
  double wdest = playtracker->processing_height * src_width / (double)src_height;
  guint dest_width, dest_height;

  if (hdest <= playtracker->processing_height) {
    dest_width = playtracker->processing_width;
    dest_height = hdest;
  } else {
    dest_width = wdest;
    dest_height = playtracker->processing_height;
  }

  // Calculate scaling ratio while maintaining aspect ratio
  ratio = MIN(1.0 * dest_width / src_width, 1.0 * dest_height / src_height);

  if ((crop_rect_params->width == 0) || (crop_rect_params->height == 0)) {
    GST_ELEMENT_ERROR(playtracker, STREAM, FAILED, ("%s:crop_rect_params dimensions are zero", __func__), (NULL));
    return GST_FLOW_ERROR;
  }
#ifdef __aarch64__
  if (ratio <= 1.0 / 16 || ratio >= 16.0) {
    // Currently cannot scale by ratio > 16 or < 1/16 for Jetson
    return GST_FLOW_ERROR;
  }
#endif

  /* We will first convert only the Region of Interest (the entire frame or the
   * object bounding box) to RGB and then scale the converted RGB frame to
   * processing resolution. */
  GST_DEBUG_OBJECT(playtracker, "Scaling and converting input buffer\n");

  /* Create temporary src and dest surfaces for NvBufSurfTransform API. */
  playtracker->batch_insurf.surfaceList[playtracker->batch_insurf.numFilled] = *src_frame;

  /* Set the source ROI. Could be entire frame or an object. */
  playtracker->transform_params.src_rect[playtracker->batch_insurf.numFilled] = {
      (guint)src_top, (guint)src_left, (guint)src_width, (guint)src_height};
  /* Set the dest ROI. Could be the entire destination frame or part of it to
   * maintain aspect ratio. */
  playtracker->transform_params.dst_rect[playtracker->batch_insurf.numFilled] = {0, 0, dest_width, dest_height};

  playtracker->batch_insurf.numFilled++;

  return GST_FLOW_OK;
}

static gboolean convert_batch_and_push_to_process_thread(GstDsPlayTracker* playtracker, GstDsPlayTrackerBatch* batch) {
  NvBufSurfTransform_Error err;
  NvBufSurfTransformConfigParams transform_config_params;
  std::string nvtx_str;

  // Configure transform session parameters for the transformation
  transform_config_params.compute_mode = playtracker->transform_config_params.compute_mode;
  transform_config_params.gpu_id = playtracker->gpu_id;
  transform_config_params.cuda_stream = playtracker->cuda_stream;

  err = NvBufSurfTransformSetSessionParams(&transform_config_params);
  if (err != NvBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR(
        playtracker, STREAM, FAILED, ("NvBufSurfTransformSetSessionParams failed with error %d", err), (NULL));
    return FALSE;
  }

  nvtxEventAttributes_t eventAttrib = {0};
  eventAttrib.version = NVTX_VERSION;
  eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
  eventAttrib.colorType = NVTX_COLOR_ARGB;
  eventAttrib.color = 0xFFFF0000;
  eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII;
  nvtx_str = "convert_buf batch_num=" + std::to_string(playtracker->current_batch_num);
  eventAttrib.message.ascii = nvtx_str.c_str();

  nvtxDomainRangePushEx(playtracker->nvtx_domain, &eventAttrib);

  g_mutex_lock(&playtracker->process_lock);

  /* Wait if buf queue is empty. */
  while (g_queue_is_empty(playtracker->buf_queue)) {
    g_cond_wait(&playtracker->buf_cond, &playtracker->process_lock);
  }

  /* Pop a buffer from the element's buf queue. */
  batch->inter_buf = (NvBufSurface*)g_queue_pop_head(playtracker->buf_queue);
  playtracker->inter_buf = batch->inter_buf;

  g_mutex_unlock(&playtracker->process_lock);

  // Memset the memory
  for (uint i = 0; i < playtracker->batch_insurf.numFilled; i++)
    NvBufSurfaceMemSet(playtracker->inter_buf, i, 0, 0);

  /* Batched tranformation. */
  err = NvBufSurfTransform(&playtracker->batch_insurf, playtracker->inter_buf, &playtracker->transform_params);

  nvtxDomainRangePop(playtracker->nvtx_domain);

  if (err != NvBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR(
        playtracker, STREAM, FAILED, ("NvBufSurfTransform failed with error %d while converting buffer", err), (NULL));
    return FALSE;
  }

  // Use openCV to remove padding and convert RGBA to BGR. Can be skipped if
  // algorithm can handle padded RGBA data.
  for (guint i = 0; i < playtracker->batch_insurf.numFilled; i++) {
    // Map the buffer so that it can be accessed by CPU
    if (NvBufSurfaceMap(playtracker->inter_buf, i, 0, NVBUF_MAP_READ) != 0) {
      GST_ELEMENT_ERROR(playtracker, STREAM, FAILED, ("%s:buffer map to be accessed by CPU failed", __func__), (NULL));
      return FALSE;
    }
    // sync mapped data for CPU access
    NvBufSurfaceSyncForCpu(playtracker->inter_buf, i, 0);

    if (NvBufSurfaceUnMap(playtracker->inter_buf, i, 0)) {
      GST_ELEMENT_ERROR(
          playtracker, STREAM, FAILED, ("%s:buffer unmap to be accessed by CPU failed", __func__), (NULL));
      return FALSE;
    }

#ifdef __aarch64__
    // To use the converted buffer in CUDA, create an EGLImage and then use
    // CUDA-EGL interop APIs
    if (USE_EGLIMAGE) {
      if (NvBufSurfaceMapEglImage(playtracker->inter_buf, 0) != 0) {
        GST_ELEMENT_ERROR(playtracker, STREAM, FAILED, ("%s:buffer map eglimage failed", __func__), (NULL));
        return FALSE;
      }
      // playtracker->inter_buf->surfaceList[0].mappedAddr.eglImage
      // Use interop APIs cuGraphicsEGLRegisterImage and
      // cuGraphicsResourceGetMappedEglFrame to access the buffer in CUDA

      // Destroy the EGLImage
      NvBufSurfaceUnMapEglImage(playtracker->inter_buf, 0);
    }
#endif
  }

  /* Push the batch info structure in the processing queue and notify the
   * process thread that a new batch has been queued. */
  g_mutex_lock(&playtracker->process_lock);

  g_queue_push_tail(playtracker->process_queue, batch);
  g_cond_broadcast(&playtracker->process_cond);

  g_mutex_unlock(&playtracker->process_lock);

  return TRUE;
}

/**
 * Called when element recieves an input buffer from upstream element.
 */
static GstFlowReturn gst_playtracker_submit_input_buffer(GstBaseTransform* btrans, gboolean discont, GstBuffer* inbuf) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
  GstMapInfo in_map_info;
  NvBufSurface* in_surf;
  GstDsPlayTrackerBatch* buf_push_batch;
  GstFlowReturn flow_ret;
  std::string nvtx_str;
  std::unique_ptr<GstDsPlayTrackerBatch> batch = nullptr;

  NvDsBatchMeta* batch_meta = NULL;
  // guint i = 0;
  gdouble scale_ratio = 1.0;
  guint num_filled = 0;

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, playtracker->gpu_id);

  playtracker->current_batch_num++;

  nvtxEventAttributes_t eventAttrib = {0};
  eventAttrib.version = NVTX_VERSION;
  eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
  eventAttrib.colorType = NVTX_COLOR_ARGB;
  eventAttrib.color = 0xFFFF0000;
  eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII;
  nvtx_str = "buffer_process batch_num=" + std::to_string(playtracker->current_batch_num);
  eventAttrib.message.ascii = nvtx_str.c_str();
  nvtxRangeId_t buf_process_range = nvtxDomainRangeStartEx(playtracker->nvtx_domain, &eventAttrib);

  memset(&in_map_info, 0, sizeof(in_map_info));

  /* Map the buffer contents and get the pointer to NvBufSurface. */
  if (!gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
    GST_ELEMENT_ERROR(
        playtracker, STREAM, FAILED, ("%s:gst buffer map to get pointer to NvBufSurface failed", __func__), (NULL));
    return GST_FLOW_ERROR;
  }
  in_surf = (NvBufSurface*)in_map_info.data;

  nvds_set_input_system_timestamp(inbuf, GST_ELEMENT_NAME(playtracker));

  batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  if (batch_meta == nullptr) {
    GST_ELEMENT_ERROR(playtracker, STREAM, FAILED, ("NvDsBatchMeta not found for input buffer."), (NULL));
    return GST_FLOW_ERROR;
  }
  num_filled = batch_meta->num_frames_in_batch;

  for (guint i = 0; i < num_filled; i++) {
    NvOSD_RectParams rect_params;

    // Scale the entire frame to processing resolution
    rect_params.left = 0;
    rect_params.top = 0;
    rect_params.width = in_surf->surfaceList[i].width;
    rect_params.height = in_surf->surfaceList[i].height;

    // Scale the frame maintaining aspect ratio
    if (scale_and_fill_data(
            playtracker,
            in_surf->surfaceList + i,
            &rect_params,
            scale_ratio,
            playtracker->video_info.width,
            playtracker->video_info.height) != GST_FLOW_OK) {
      goto error;
    }

    if (batch == nullptr) {
      batch.reset(new GstDsPlayTrackerBatch);
      batch->push_buffer = FALSE;
      batch->inbuf = inbuf;
      batch->inbuf_batch_num = playtracker->current_batch_num;
    }

    /* Adding a frame to the current batch. Set the frames members. */
    GstDsPlayTrackerFrame frame;
    frame.scale_ratio_x = scale_ratio;
    frame.scale_ratio_y = scale_ratio;
    frame.obj_meta = nullptr;
    frame.frame_meta = nvds_get_nth_frame_meta(batch_meta->frame_meta_list, i);
    frame.frame_num = frame.frame_meta->frame_num;
    frame.batch_index = i;
    frame.input_surf_params = in_surf->surfaceList + i;
    batch->frames.push_back(frame);

    // Set the transform session parameters for the conversions executed in
    // this thread.
    if (batch->frames.size() == playtracker->max_batch_size || i == num_filled) {
      if (!convert_batch_and_push_to_process_thread(playtracker, batch.get())) {
        return GST_FLOW_ERROR;
      }
      /* Batch submitted. Set batch to nullptr so that a new GstDsPlayTrackerBatch
       * structure can be allocated if required. */
      batch.release();
      playtracker->batch_insurf.numFilled = 0;
    }
  }
  /* Submit a non-full batch. */
  if (batch) {
    if (!convert_batch_and_push_to_process_thread(playtracker, batch.get())) {
      return GST_FLOW_ERROR;
    }
    batch.release();
    playtracker->batch_insurf.numFilled = 0;
  }

  nvtxDomainRangeEnd(playtracker->nvtx_domain, buf_process_range);

  /* Queue a push buffer batch. This batch is not inferred. This batch is to
   * signal the process thread that there are no more batches
   * belonging to this input buffer and this GstBuffer can be pushed to
   * downstream element once all the previous processing is done. */
  buf_push_batch = new GstDsPlayTrackerBatch;
  buf_push_batch->inbuf = inbuf;
  buf_push_batch->push_buffer = TRUE;
  buf_push_batch->nvtx_complete_buf_range = buf_process_range;

  g_mutex_lock(&playtracker->process_lock);
  /* Check if this is a push buffer or event marker batch. If yes, no need to
   * queue the input for inferencing. */
  if (buf_push_batch->push_buffer) {
    /* Push the batch info structure in the processing queue and notify the
     * process thread that a new batch has been queued. */
    g_queue_push_tail(playtracker->process_queue, buf_push_batch);
    g_cond_broadcast(&playtracker->process_cond);
  }
  g_mutex_unlock(&playtracker->process_lock);

  flow_ret = GST_FLOW_OK;

error:
  gst_buffer_unmap(inbuf, &in_map_info);
  return flow_ret;
}

/**
 * If submit_input_buffer is implemented, it is mandatory to implement
 * generate_output. Buffers are not pushed to the downstream element from here.
 * Return the GstFlowReturn value of the latest pad push so that any error might
 * be caught by the application.
 */
static GstFlowReturn gst_playtracker_generate_output(GstBaseTransform* btrans, GstBuffer** outbuf) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
  return playtracker->last_flow_ret;
}

/**
 * Output loop used to pop output from processing thread, attach the output to
 * the buffer in form of NvDsMeta and push the buffer to downstream element.
 */
static gpointer gst_playtracker_output_loop(gpointer data) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(data);
  // DsPlayTrackerOutput* output;
  // NvDsObjectMeta* obj_meta = NULL;
  // gdouble scale_ratio = 1.0;

  nvtxEventAttributes_t eventAttrib = {0};
  eventAttrib.version = NVTX_VERSION;
  eventAttrib.size = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
  eventAttrib.colorType = NVTX_COLOR_ARGB;
  eventAttrib.color = 0xFFFF0000;
  eventAttrib.messageType = NVTX_MESSAGE_TYPE_ASCII;
  std::string nvtx_str;

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, playtracker->gpu_id);

  nvtx_str = "gst-playtracker_output-loop_uid=" + std::to_string(playtracker->unique_id);

  g_mutex_lock(&playtracker->process_lock);

  /* Run till signalled to stop. */
  while (!playtracker->stop) {
    std::unique_ptr<GstDsPlayTrackerBatch> batch = nullptr;

    /* Wait if processing queue is empty. */
    if (g_queue_is_empty(playtracker->process_queue)) {
      g_cond_wait(&playtracker->process_cond, &playtracker->process_lock);
      continue;
    }

    /* Pop a batch from the element's process queue. */
    batch.reset((GstDsPlayTrackerBatch*)g_queue_pop_head(playtracker->process_queue));
    g_cond_broadcast(&playtracker->process_cond);

    /* Event marker used for synchronization. No need to process further. */
    if (batch->event_marker) {
      continue;
    }

    g_mutex_unlock(&playtracker->process_lock);

    /* Need to only push buffer to downstream element. This batch was not
     * actually submitted for inferencing. */
    if (batch->push_buffer) {
      nvtxDomainRangeEnd(playtracker->nvtx_domain, batch->nvtx_complete_buf_range);

      nvds_set_output_system_timestamp(batch->inbuf, GST_ELEMENT_NAME(playtracker));

      GstFlowReturn flow_ret = gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(playtracker), batch->inbuf);
      if (playtracker->last_flow_ret != flow_ret) {
        switch (flow_ret) {
            /* Signal the application for pad push errors by posting a error
             * message on the pipeline bus. */
          case GST_FLOW_ERROR:
          case GST_FLOW_NOT_LINKED:
          case GST_FLOW_NOT_NEGOTIATED:
            GST_ELEMENT_ERROR(
                playtracker,
                STREAM,
                FAILED,
                ("Internal data stream error."),
                ("streaming stopped, reason %s (%d)", gst_flow_get_name(flow_ret), flow_ret));
            break;
          default:
            break;
        }
      }
      playtracker->last_flow_ret = flow_ret;
      g_mutex_lock(&playtracker->process_lock);
      continue;
    }

    nvtx_str = "dequeueOutputAndAttachMeta batch_num=" + std::to_string(batch->inbuf_batch_num);
    eventAttrib.message.ascii = nvtx_str.c_str();
    nvtxDomainRangePushEx(playtracker->nvtx_domain, &eventAttrib);

    /* For each frame attach metadata output. */
    for (guint i = 0; i < batch->frames.size(); i++) {
      GstDsPlayTrackerFrame& frame = batch->frames[i];
      DsPlayTrackerProcessFrame(frame, playtracker->playtrackerlib_ctx);

      // Process to get the output
      // output = DsPlayTrackerProcess(
      //     playtracker->playtrackerlib_ctx, (unsigned char*)batch->inter_buf->surfaceList[i].mappedAddr.addr[0]);
      // // Attach the metadata for the full frame
      // // attach_metadata_full_frame(playtracker, batch->frames[i].frame_meta, scale_ratio, output, i);
      // free(output);
    }

    g_mutex_lock(&playtracker->process_lock);

    g_queue_push_tail(playtracker->buf_queue, batch->inter_buf);
    g_cond_broadcast(&playtracker->buf_cond);

    nvtxDomainRangePop(playtracker->nvtx_domain);
  }
  g_mutex_unlock(&playtracker->process_lock);

  return nullptr;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean playtracker_plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_playtracker_debug, "playtracker", 0, "playtracker plugin");

  return gst_element_register(plugin, "playtracker", GST_RANK_PRIMARY, GST_TYPE_DSPLAYTRACKER);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    playtracker,
    DESCRIPTION,
    playtracker_plugin_init,
    "7.1",
    LICENSE,
    BINARY_PACKAGE,
    URL)
