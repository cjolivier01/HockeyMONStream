/*
 * SPDX-FileCopyrightText: Copyright (c) 2020-2024 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 * SPDX-License-Identifier: LicenseRef-NvidiaProprietary
 *
 * NVIDIA CORPORATION, its affiliates and licensors retain all intellectual
 * property and proprietary rights in and to this material, related
 * documentation and any modifications thereto. Any use, reproduction,
 * disclosure or distribution of this material and related documentation
 * without an express license agreement from NVIDIA CORPORATION or
 * its affiliates is strictly prohibited.
 */

/**
 * There are two threads in the optimized code. input thread and Processing thread.
 * The pre-procesing as required by the algorithm like scaling and color
 * conversion of data is done in input thread. This is done using NvBufSurfTransform's
 * batch conversion APIs to improve performance. The processing of data using custom
 * algorithm and parsing the output and  metadata attachment is done in separate processing
 * thread.
 *
 * There are two queues used for buffering and transferring data between thread:
 * Process_queue and buf_queue Process_queue is used to send filled batched data to
 * process thread and buf_queue is used to get return empty processed buffers from
 * process thread to input thread.  Two buffers are used in a ping pong manner between
 * the two threads for parallel processing.
 */

#include <string.h>
#include <fstream>
#include <iostream>
#include <ostream>
#include <sstream>
#include <string>

#include "gstdscameraman_optimized.h"

#include <sys/time.h>
#include <condition_variable>
#include <mutex>
#include <thread>

GST_DEBUG_CATEGORY_STATIC(gst_dscameraman_debug);
#define GST_CAT_DEFAULT gst_dscameraman_debug
#define USE_EGLIMAGE 1

static GQuark _dsmeta_quark = 0;

/* Enum to identify properties */
enum {
  PROP_0,
  PROP_UNIQUE_ID,
  PROP_PROCESSING_WIDTH,
  PROP_PROCESSING_HEIGHT,
  PROP_PROCESS_FULL_FRAME,
  PROP_BATCH_SIZE,
  PROP_GPU_DEVICE_ID
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
#define DEFAULT_PROCESS_FULL_FRAME TRUE
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
static GstStaticPadTemplate gst_dscameraman_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_dscameraman_src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_dscameraman_parent_class parent_class
G_DEFINE_TYPE(GstDsCameraMan, gst_dscameraman, GST_TYPE_BASE_TRANSFORM);

static void gst_dscameraman_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gst_dscameraman_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

static gboolean gst_dscameraman_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps);
static gboolean gst_dscameraman_start(GstBaseTransform* btrans);
static gboolean gst_dscameraman_stop(GstBaseTransform* btrans);

static GstFlowReturn pad_chain(GstPad* pad, GstObject* parent, GstBuffer* buf);

// Forward declarations
static GstFlowReturn gst_crop_buf_surface_transform_ip(GstBaseTransform* base, GstBuffer* buf);

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void gst_dscameraman_class_init(GstDsCameraManClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseTransformClass* gstbasetransform_class;

  // Indicates we want to use DS buf api
  g_setenv("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;
  gstbasetransform_class = (GstBaseTransformClass*)klass;

  /* Overide base class functions */
  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_dscameraman_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_dscameraman_get_property);

  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR(gst_dscameraman_set_caps);
  gstbasetransform_class->start = GST_DEBUG_FUNCPTR(gst_dscameraman_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR(gst_dscameraman_stop);

  gstbasetransform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_crop_buf_surface_transform_ip);

  // gstbasetransform_class->submit_input_buffer = GST_DEBUG_FUNCPTR(gst_dscameraman_submit_input_buffer);
  // gstbasetransform_class->generate_output = GST_DEBUG_FUNCPTR(gst_dscameraman_generate_output);

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
      PROP_PROCESS_FULL_FRAME,
      g_param_spec_boolean(
          "full-frame",
          "Full frame",
          "Enable to process full frame or disable to process objects detected"
          "by primary detector",
          DEFAULT_PROCESS_FULL_FRAME,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_BATCH_SIZE,
      g_param_spec_uint(
          "batch-size",
          "Batch Size",
          "Maximum batch size for processing",
          1,
          NVDSCAMERAMAN_MAX_BATCH_SIZE,
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
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_dscameraman_src_template));
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_dscameraman_sink_template));

  /* Set metadata describing the element */
  gst_element_class_set_details_simple(
      gstelement_class,
      "DsCameraMan plugin",
      "DsCameraMan Plugin",
      "Process a 3rdparty example algorithm on objects / full frame",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");
}

static void gst_dscameraman_init(GstDsCameraMan* dscameraman) {
  GstBaseTransform* btrans = GST_BASE_TRANSFORM(dscameraman);
  GstElement* element = GST_ELEMENT(dscameraman);

  /* We will not be generating a new buffer. Just adding / updating
   * metadata. */
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(btrans), TRUE);
  /* We do not want to change the input caps. Set to passthrough. transform_ip
   * is still called. */
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(btrans), TRUE);

  /* Initialize all property variables to default values */
  dscameraman->unique_id = DEFAULT_UNIQUE_ID;
  dscameraman->processing_width = DEFAULT_PROCESSING_WIDTH;
  dscameraman->processing_height = DEFAULT_PROCESSING_HEIGHT;
  dscameraman->process_full_frame = DEFAULT_PROCESS_FULL_FRAME;
  dscameraman->gpu_id = DEFAULT_GPU_ID;
  dscameraman->max_batch_size = DEFAULT_BATCH_SIZE;

  // Set the chain function for the sink pad
  // auto sinkpad = gst_element_get_static_pad(element, "sink");
  // gst_pad_set_chain_function(sinkpad, pad_chain);

  /* This quark is required to identify NvDsMeta when iterating through
   * the buffer metadatas */
  if (!_dsmeta_quark)
    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void gst_dscameraman_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  GstDsCameraMan* dscameraman = GST_DSCAMERAMAN(object);
  switch (prop_id) {
    case PROP_UNIQUE_ID:
      dscameraman->unique_id = g_value_get_uint(value);
      break;
    case PROP_PROCESSING_WIDTH:
      dscameraman->processing_width = g_value_get_int(value);
      break;
    case PROP_PROCESSING_HEIGHT:
      dscameraman->processing_height = g_value_get_int(value);
      break;
    case PROP_PROCESS_FULL_FRAME:
      dscameraman->process_full_frame = g_value_get_boolean(value);
      break;
    case PROP_GPU_DEVICE_ID:
      dscameraman->gpu_id = g_value_get_uint(value);
      break;
    case PROP_BATCH_SIZE:
      dscameraman->max_batch_size = g_value_get_uint(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void gst_dscameraman_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  GstDsCameraMan* dscameraman = GST_DSCAMERAMAN(object);

  switch (prop_id) {
    case PROP_UNIQUE_ID:
      g_value_set_uint(value, dscameraman->unique_id);
      break;
    case PROP_PROCESSING_WIDTH:
      g_value_set_int(value, dscameraman->processing_width);
      break;
    case PROP_PROCESSING_HEIGHT:
      g_value_set_int(value, dscameraman->processing_height);
      break;
    case PROP_PROCESS_FULL_FRAME:
      g_value_set_boolean(value, dscameraman->process_full_frame);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint(value, dscameraman->gpu_id);
      break;
    case PROP_BATCH_SIZE:
      g_value_set_uint(value, dscameraman->max_batch_size);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/**
 * Initialize all resources and start the process thread
 */
static gboolean gst_dscameraman_start(GstBaseTransform* btrans) {
  GstDsCameraMan* dscameraman = GST_DSCAMERAMAN(btrans);
  std::string nvtx_str;
  // NvBufSurface* inter_buf;
  // NvBufSurfaceCreateParams create_params = {0};
  DsCameraManInitParams init_params = {
      dscameraman->processing_width, dscameraman->processing_height, dscameraman->process_full_frame};

  /* Algorithm specific initializations and resource allocation. */
  dscameraman->dscameramanlib_ctx = DsCameraManCtxInit(&init_params);

  GST_DEBUG_OBJECT(dscameraman, "ctx lib %p \n", dscameraman->dscameramanlib_ctx);

  nvtx_str = "GstNvDsCameraMan: UID=" + std::to_string(dscameraman->unique_id);
  auto nvtx_deleter = [](nvtxDomainHandle_t d) { nvtxDomainDestroy(d); };
  std::unique_ptr<nvtxDomainRegistration, decltype(nvtx_deleter)> nvtx_domain_ptr(
      nvtxDomainCreate(nvtx_str.c_str()), nvtx_deleter);

  CHECK_CUDA_STATUS(cudaSetDevice(dscameraman->gpu_id), "Unable to set cuda device");

  CHECK_CUDA_STATUS(cudaStreamCreate(&dscameraman->cuda_stream), "Could not create cuda stream");

  /* An intermediate buffer for NV12/RGBA to BGR conversion  will be
   * required. Can be skipped if custom algorithm can work directly on NV12/RGBA. */
  //   create_params.gpuId = dscameraman->gpu_id;
  //   create_params.width = dscameraman->processing_width;
  //   create_params.height = dscameraman->processing_height;
  //   create_params.size = 0;
  //   create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  //   create_params.layout = NVBUF_LAYOUT_PITCH;
  // #ifdef __aarch64__
  //   create_params.memType = NVBUF_MEM_DEFAULT;
  // #else
  //   create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
  // #endif

  /* Create process queue and cvmat queue to transfer data between threads.
   * We will be using this queue to maintain the list of frames/objects
   * currently given to the algorithm for processing. */
  // dscameraman->process_queue = g_queue_new();
  // dscameraman->buf_queue = g_queue_new();

  // for (int i = 0; i < 2; i++) {
  //   if (NvBufSurfaceCreate(&inter_buf, dscameraman->max_batch_size, &create_params) != 0) {
  //     GST_ERROR("Error: Could not allocate internal buffer for dscameraman");
  //     goto error;
  //   }

  //   g_queue_push_tail(dscameraman->buf_queue, inter_buf);
  // }

  /* Set the NvBufSurfTransform config parameters. */
  dscameraman->transform_config_params.compute_mode = NvBufSurfTransformCompute_Default;
  dscameraman->transform_config_params.gpu_id = dscameraman->gpu_id;

  /* Create the intermediate NvBufSurface structure for holding an array of input
   * NvBufSurfaceParams for batched transforms. */
  dscameraman->batch_insurf.surfaceList = new NvBufSurfaceParams[dscameraman->max_batch_size];
  dscameraman->batch_insurf.batchSize = dscameraman->max_batch_size;
  dscameraman->batch_insurf.gpuId = dscameraman->gpu_id;

  /* Set up the NvBufSurfTransformParams structure for batched transforms. */
  dscameraman->transform_params.src_rect = new NvBufSurfTransformRect[dscameraman->max_batch_size];
  dscameraman->transform_params.dst_rect = new NvBufSurfTransformRect[dscameraman->max_batch_size];
  dscameraman->transform_params.transform_flag =
      NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST;
  dscameraman->transform_params.transform_flip = NvBufSurfTransform_None;
  dscameraman->transform_params.transform_filter = NvBufSurfTransformInter_Default;

  dscameraman->nvtx_domain = nvtx_domain_ptr.release();

  return TRUE;
error:

  delete[] dscameraman->transform_params.src_rect;
  delete[] dscameraman->transform_params.dst_rect;
  delete[] dscameraman->batch_insurf.surfaceList;

  if (dscameraman->cuda_stream) {
    cudaStreamDestroy(dscameraman->cuda_stream);
    dscameraman->cuda_stream = NULL;
  }
  if (dscameraman->dscameramanlib_ctx)
    DsCameraManCtxDeinit(dscameraman->dscameramanlib_ctx);
  return FALSE;
}

/**
 * Stop the process thread and free up all the resources
 */
static gboolean gst_dscameraman_stop(GstBaseTransform* btrans) {
  GstDsCameraMan* dscameraman = GST_DSCAMERAMAN(btrans);

  // NvBufSurface* inter_buf;

  // g_mutex_lock(&dscameraman->process_lock);

  /* Wait till all the items in the queue are handled. */
  // while (!g_queue_is_empty(dscameraman->process_queue)) {
  //   g_cond_wait(&dscameraman->process_cond, &dscameraman->process_lock);
  // }

  // while (!g_queue_is_empty(dscameraman->buf_queue)) {
  //   inter_buf = (NvBufSurface*)g_queue_pop_head(dscameraman->buf_queue);
  //   if (inter_buf)
  //     NvBufSurfaceDestroy(inter_buf);
  //   inter_buf = NULL;
  // }
  dscameraman->stop = TRUE;

  // g_cond_broadcast(&dscameraman->process_cond);
  // g_mutex_unlock(&dscameraman->process_lock);

  // g_thread_join(dscameraman->process_thread);

  if (dscameraman->cuda_stream)
    cudaStreamDestroy(dscameraman->cuda_stream);
  dscameraman->cuda_stream = NULL;

  delete[] dscameraman->transform_params.src_rect;
  delete[] dscameraman->transform_params.dst_rect;
  delete[] dscameraman->batch_insurf.surfaceList;

  // Deinit the algorithm library
  DsCameraManCtxDeinit(dscameraman->dscameramanlib_ctx);
  dscameraman->dscameramanlib_ctx = NULL;

  GST_DEBUG_OBJECT(dscameraman, "ctx lib released \n");

  // g_queue_free(dscameraman->process_queue);

  // g_queue_free(dscameraman->buf_queue);

  return TRUE;
}

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean gst_dscameraman_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps) {
  GstDsCameraMan* dscameraman = GST_DSCAMERAMAN(btrans);
  /* Save the input video information, since this will be required later. */
  gst_video_info_from_caps(&dscameraman->video_info, incaps);

  CHECK_CUDA_STATUS(cudaSetDevice(dscameraman->gpu_id), "Unable to set cuda device");

  return TRUE;

error:
  return FALSE;
}

void printNvBufSurfaceParams(const NvBufSurfaceParams& params) {
  std::cout << "NvBufSurfaceParams Details:" << std::endl;
  std::cout << "Width: " << params.width << std::endl;
  std::cout << "Height: " << params.height << std::endl;
  std::cout << "Pitch: " << params.pitch << std::endl;
  std::cout << "Color Format: " << params.colorFormat << std::endl;
  std::cout << "Layout: " << params.layout << std::endl;
  std::cout << "Buffer Size: " << params.dataSize << std::endl;
  std::cout << "Mapped Address: " << params.dataPtr << std::endl;
  // std::cout << "Device Memory Address: " << params.gpuId << std::endl;
  // std::cout << "Memory Type: " << params.memType << std::endl;
}

static GstFlowReturn pad_chain(GstPad* pad, GstObject* parent, GstBuffer* buf) {
  // GstBuffer* new_buf;

  // // Create a new buffer or modify the existing one.
  // new_buf = gst_buffer_new_allocate(NULL, gst_buffer_get_size(buf), NULL);

  // if (!new_buf) {
  //   GST_ERROR("Failed to allocate new GstBuffer");
  //   gst_buffer_unref(buf); // Release the original buffer before returning
  //   return GST_FLOW_ERROR;
  // }

  // // Optionally copy or modify metadata from the original buffer to the new buffer
  // GST_BUFFER_PTS(new_buf) = GST_BUFFER_PTS(buf);
  // GST_BUFFER_DTS(new_buf) = GST_BUFFER_DTS(buf);

  // // Push the new buffer downstream
  //GstPad* peer = gst_pad_get_peer(pad);
  //if (peer != NULL) {
    //gst_pad_push(peer, buf);
  //   gst_pad_push(peer, new_buf);
  //   // gst_object_unref(peer);
  //}

  // // Unreference the original buffer to allow GStreamer to return it to streammux
  // gst_buffer_unref(buf);

  return GST_FLOW_OK;
}

static GstFlowReturn gst_crop_buf_surface_transform_ip(GstBaseTransform* btrans, GstBuffer* inbuf) {
  GstDsCameraMan* dscameraman = GST_DSCAMERAMAN(btrans);
  DsCameraManOutput* cm_results;
  GstMapInfo in_map_info;
  GstFlowReturn flow_ret = GST_FLOW_ERROR;

  NvBufSurface* surface = NULL;
  NvDsBatchMeta* batch_meta = NULL;
  NvDsFrameMeta* frame_meta = NULL;
  NvDsMetaList* l_frame = NULL;
  size_t frame_nr = 0;

  // NvBufSurfaceCreateParams create_params = {0};

  dscameraman->frame_num++;
  CHECK_CUDA_STATUS(cudaSetDevice(dscameraman->gpu_id), "Unable to set cuda device");

  memset(&in_map_info, 0, sizeof(in_map_info));
  if (!gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
    g_print("Error: Failed to map gst buffer\n");
    goto error;
  }

  nvds_set_input_system_timestamp(inbuf, GST_ELEMENT_NAME(dscameraman));
  surface = (NvBufSurface*)in_map_info.data;
  GST_DEBUG_OBJECT(dscameraman, "Processing Frame %" G_GUINT64_FORMAT " Surface %p\n", dscameraman->frame_num, surface);

  if (CHECK_NVDS_MEMORY_AND_GPUID(dscameraman, surface))
    goto error;

  batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  if (batch_meta == nullptr) {
    GST_ELEMENT_ERROR(dscameraman, STREAM, FAILED, ("NvDsBatchMeta not found for input buffer."), (NULL));
    return GST_FLOW_ERROR;
  }

  /* Using object crops as input to the algorithm. The objects are detected by
   * the primary detector */

  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next, ++frame_nr) {
    frame_meta = (NvDsFrameMeta*)(l_frame->data);
    cm_results = DsCameraManProcess(frame_meta, dscameraman->dscameramanlib_ctx);

    NvBufSurfaceParams surface_params = surface->surfaceList[frame_nr];
    (void)surface_params;
    // printNvBufSurfaceParams(surface_params);

    //   create_params.gpuId = dscameraman->gpu_id;
    //   create_params.width = dscameraman->processing_width;
    //   create_params.height = dscameraman->processing_height;
    //   create_params.size = 0;
    //   create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
    //   create_params.layout = NVBUF_LAYOUT_PITCH;
    // #ifdef __aarch64__
    //   create_params.memType = NVBUF_MEM_DEFAULT;
    // #else
    //   create_params.memType = NVBUF_MEM_CUDA_UNIFIED;
    // #endif

    /* Create process queue and cvmat queue to transfer data between threads.
     * We will be using this queue to maintain the list of frames/objects
     * currently given to the algorithm for processing. */
    // dscameraman->process_queue = g_queue_new();
    // dscameraman->buf_queue = g_queue_new();

    // for (int i = 0; i < 2; i++) {
    //   if (NvBufSurfaceCreate(&inter_buf, dscameraman->max_batch_size, &create_params) != 0) {
    //     GST_ERROR("Error: Could not allocate internal buffer for dscameraman");
    //     goto error;
    //   }

    //   g_queue_push_tail(dscameraman->buf_queue, inter_buf);
    // }

    free(cm_results);
  }
  assert(frame_nr == surface->numFilled);
  flow_ret = GST_FLOW_OK;

error:

  nvds_set_output_system_timestamp(inbuf, GST_ELEMENT_NAME(dscameraman));
  gst_buffer_unmap(inbuf, &in_map_info);
  return flow_ret;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean dscameraman_plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_dscameraman_debug, "dscameraman", 0, "dscameraman plugin");

  return gst_element_register(plugin, "dscameraman", GST_RANK_PRIMARY, GST_TYPE_DSCAMERAMAN);
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvdsgst_dscameraman,
    DESCRIPTION,
    dscameraman_plugin_init,
    "7.1",
    LICENSE,
    BINARY_PACKAGE,
    URL)
