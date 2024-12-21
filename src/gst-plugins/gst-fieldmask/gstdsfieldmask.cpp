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

#include "gstdsfieldmask.h"
#include <string.h>
#include <sys/time.h>
#include <cassert>
#include <string>

namespace {
GST_DEBUG_CATEGORY_STATIC(gst_dsfieldmask_debug);
#define GST_CAT_DEFAULT gst_dsfieldmask_debug
#define USE_EGLIMAGE 1
/* enable to write transformed cvmat to files */
/* #define DSEXAMPLE_DEBUG */
static GQuark _dsmeta_quark = 0;

/* Enum to identify properties */
enum {
  PROP_0,
  PROP_UNIQUE_ID,
  PROP_PROCESSING_WIDTH,
  PROP_PROCESSING_HEIGHT,
  PROP_PROCESS_FULL_FRAME,
  PROP_BATCH_SIZE,
  PROP_BLUR_OBJECTS,
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
#define DEFAULT_PROCESS_FULL_FRAME TRUE
#define DEFAULT_BLUR_OBJECTS FALSE
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
static GstStaticPadTemplate gst_dsfieldmask_sink_template = GST_STATIC_PAD_TEMPLATE(
    "sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

static GstStaticPadTemplate gst_dsfieldmask_src_template = GST_STATIC_PAD_TEMPLATE(
    "src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(GST_VIDEO_CAPS_MAKE_WITH_FEATURES(GST_CAPS_FEATURE_MEMORY_NVMM, "{ NV12, RGBA, I420 }")));

/* Define our element type. Standard GObject/GStreamer boilerplate stuff */
#define gst_dsfieldmask_parent_class parent_class
G_DEFINE_TYPE(GstDsFieldMask, gst_dsfieldmask, GST_TYPE_BASE_TRANSFORM);

static void gst_dsfieldmask_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec);
static void gst_dsfieldmask_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec);

static gboolean gst_dsfieldmask_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps);
static gboolean gst_dsfieldmask_start(GstBaseTransform* btrans);
static gboolean gst_dsfieldmask_stop(GstBaseTransform* btrans);

static GstFlowReturn gst_dsfieldmask_transform_ip(GstBaseTransform* btrans, GstBuffer* inbuf);

namespace {
struct InputParams {
  NvBufSurface* pSurfaceBatch;
  NvDsBatchMeta* pBatchMeta;
  void* pPreservedData;
  bool eventMarker;
};
} // namespace

#if 0
static GstFlowReturn gst_ds_example_submit_input_buffer(
    GstBaseTransform* trans,
    gboolean is_discont,
    GstBuffer* inbuf) {
  GstDsFieldMask* dsfieldmask = (GstDsFieldMask*)trans;
  (void)dsfieldmask;

  /** NOTE: Initializing state to nullptr is essential. */
  NvDsBatchMeta* batch_meta = nullptr;
  GstMapInfo inmap;

  batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);

  if (batch_meta->num_frames_in_batch == 0) {
    // g_mutex_lock(&dsfieldmask->eventLock);
    // bool result = dsfieldmask->trackerIface->flushReqs();
    // g_cond_wait(&dsfieldmask->eventCondition, &dsfieldmask->eventLock);
    // g_mutex_unlock(&dsfieldmask->eventLock);
    // if (!result)
    // {
    //   return GST_FLOW_ERROR;
    // }
    return gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(trans), inbuf);
  }

  memset(&inmap, 0, sizeof(inmap));
  if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ)) {
    return GST_FLOW_ERROR;
  }

  nvds_set_input_system_timestamp(inbuf, GST_ELEMENT_NAME(trans));

  NvBufSurface* inputBuffer = reinterpret_cast<NvBufSurface*>(inmap.data);
  (void)inputBuffer;
  gst_buffer_unmap(inbuf, &inmap);

  /* Compose the input params and submit for tracker processing
     Keep track of the inbuf via pPreservedData, so the output loop
     can push it down the pipeline. */
  InputParams input;
  input.pSurfaceBatch = inputBuffer;
  input.pBatchMeta = batch_meta;
  input.pPreservedData = inbuf;
  input.eventMarker = false;
  (void)input;

  if (((inputBuffer->memType == NVBUF_MEM_DEFAULT ||
        inputBuffer->memType == NVBUF_MEM_CUDA_DEVICE) &&
       ((int)inputBuffer->gpuId != (int)dsfieldmask->gpu_id)) ||
      (((int)inputBuffer->gpuId == (int)dsfieldmask->gpu_id) &&
       (inputBuffer->memType == NVBUF_MEM_SYSTEM))) {
    GST_ELEMENT_ERROR(
        dsfieldmask,
        RESOURCE,
        FAILED,
        ("Memory Compatibility Error:Input surface gpu-id doesnt match with configured gpu-id for element,"
         " please allocate input using unified memory, or use same gpu-ids OR,"
         " if same gpu-ids are used ensure appropriate Cuda memories are used"),
        ("surface-gpu-id=%d,%s-gpu-id=%d",
         inputBuffer->gpuId,
         GST_ELEMENT_NAME(dsfieldmask),
         dsfieldmask->gpu_id));
    return GST_FLOW_ERROR;
  }

  // /** Check frame count in batch doesn't exceed batch size */
  if (dsfieldmask->batch_size &&
      input.pBatchMeta->num_frames_in_batch > dsfieldmask->batch_size) {
    GST_ELEMENT_ERROR(
        dsfieldmask,
        STREAM,
        FAILED,
        ("Frame number in input batch exceeds maximum batch size"),
        (nullptr));
    return GST_FLOW_ERROR;
  }

  // if (!dsfieldmask->trackerIface->submitInput(input))
  // {
  //   GST_ELEMENT_ERROR (dsfieldmask, STREAM, FAILED,
  //     ("Failed to submit input to tracker"),
  //     (nullptr));
  //   return GST_FLOW_ERROR;
  // }

  return gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(trans), inbuf);

  // return GST_FLOW_OK;
}

// static gpointer gst_nv_nvtracker_output_loop (gpointer user_data)
// {
//   GstNvTracker *dsfieldmask = (GstNvTracker *) user_data;
//   while(dsfieldmask->running)
//   {
//     InputParams inputParams;
//     CompletionStatus status =
//     dsfieldmask->trackerIface->waitForCompletion(inputParams); if (status ==
//     CompletionStatus_OK && dsfieldmask->running)
//     {
//       /** Check for event marker */
//       if (inputParams.eventMarker) {
//           g_mutex_lock(&dsfieldmask->eventLock);
//           g_cond_signal(&dsfieldmask->eventCondition);
//           g_mutex_unlock(&dsfieldmask->eventLock);
//           continue;
//       }
//       GstBuffer *inbuf = (GstBuffer*)inputParams.pPreservedData;

//       nvds_set_output_system_timestamp(inbuf, GST_ELEMENT_NAME(dsfieldmask));

//       /** Push the buffer to peer sink pad */
//       gst_pad_push (GST_BASE_TRANSFORM_SRC_PAD (dsfieldmask), inbuf);

//     } else if (status == CompletionStatus_Exit) {
//       dsfieldmask->running = false;
//       return dsfieldmask;
//     }
//   }

//   return dsfieldmask;
// }

/* Mandatory override of generate_output function to match submit_input.
 * The actual output is pushed from gst_nv_nvtracker_output_loop.
 */
static GstFlowReturn gst_ds_example_generate_output(
    GstBaseTransform* trans,
    GstBuffer** outbuf) {
  *outbuf = NULL;
  return GST_FLOW_OK;
}
#endif

/* Install properties, set sink and src pad capabilities, override the required
 * functions of the base class, These are common to all instances of the
 * element.
 */
static void gst_dsfieldmask_class_init(GstDsFieldMaskClass* klass) {
  GObjectClass* gobject_class;
  GstElementClass* gstelement_class;
  GstBaseTransformClass* gstbasetransform_class;
  /* Indicates we want to use DS buf api */
  g_setenv("DS_NEW_BUFAPI", "1", TRUE);

  gobject_class = (GObjectClass*)klass;
  gstelement_class = (GstElementClass*)klass;
  gstbasetransform_class = (GstBaseTransformClass*)klass;

  /* Overide base class functions */
  gobject_class->set_property = GST_DEBUG_FUNCPTR(gst_dsfieldmask_set_property);
  gobject_class->get_property = GST_DEBUG_FUNCPTR(gst_dsfieldmask_get_property);

  // gstbasetransform_class->submit_input_buffer =
  //     GST_DEBUG_FUNCPTR(gst_ds_example_submit_input_buffer);
  // gstbasetransform_class->generate_output =
  //     GST_DEBUG_FUNCPTR(gst_ds_example_generate_output);

  gstbasetransform_class->set_caps = GST_DEBUG_FUNCPTR(gst_dsfieldmask_set_caps);
  gstbasetransform_class->start = GST_DEBUG_FUNCPTR(gst_dsfieldmask_start);
  gstbasetransform_class->stop = GST_DEBUG_FUNCPTR(gst_dsfieldmask_stop);

  //   /** NOTE: Initializing state to nullptr is essential. */
  // NvDsBatchMeta *batch_meta = nullptr;
  // GstMapInfo inmap;

  // batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);

  gstbasetransform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_dsfieldmask_transform_ip);

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
      PROP_DETECTION_MASK_FILE,
      g_param_spec_string(
          "detection-mask",
          "Detection Mask",
          "Restrict detections to position mask",
          /*default_value=*/"",
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS)));

  g_object_class_install_property(
      gobject_class,
      PROP_BLUR_OBJECTS,
      g_param_spec_boolean(
          "blur-objects",
          "Blur Objects",
          "Enable to blur the objects detected in full-frame=0 mode"
          "by primary detector",
          DEFAULT_BLUR_OBJECTS,
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

  g_object_class_install_property(
      gobject_class,
      PROP_BATCH_SIZE,
      g_param_spec_uint(
          "batch-size",
          "Batch Size",
          "Maximum batch size for processing",
          1,
          G_MAXUINT,
          DEFAULT_BATCH_SIZE,
          (GParamFlags)(G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS | GST_PARAM_MUTABLE_READY)));
  /* Set sink and src pad capabilities */
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_dsfieldmask_src_template));
  gst_element_class_add_pad_template(gstelement_class, gst_static_pad_template_get(&gst_dsfieldmask_sink_template));

  /* Set metadata describing the element */
  gst_element_class_set_details_simple(
      gstelement_class,
      "DsFieldMask plugin",
      "DsFieldMask Plugin",
      "Process a 3rdparty example algorithm on objects / full frame",
      "NVIDIA Corporation. Post on Deepstream for Tesla forum for any queries "
      "@ https://devtalk.nvidia.com/default/board/209/");
}

static void gst_dsfieldmask_init(GstDsFieldMask* dsfieldmask) {
  // std::cerr << "element ASSERTING" << std::endl;
  // assert(false);
  GstBaseTransform* btrans = GST_BASE_TRANSFORM(dsfieldmask);

  /* We will not be generating a new buffer. Just adding / updating
   * metadata. */
  gst_base_transform_set_in_place(GST_BASE_TRANSFORM(btrans), TRUE);
  /* We do not want to change the input caps. Set to passthrough. transform_ip
   * is still called. */
  gst_base_transform_set_passthrough(GST_BASE_TRANSFORM(btrans), TRUE);

  /* Initialize all property variables to default values */
  dsfieldmask->unique_id = DEFAULT_UNIQUE_ID;
  dsfieldmask->processing_width = DEFAULT_PROCESSING_WIDTH;
  dsfieldmask->processing_height = DEFAULT_PROCESSING_HEIGHT;
  dsfieldmask->process_full_frame = DEFAULT_PROCESS_FULL_FRAME;
  dsfieldmask->blur_objects = DEFAULT_BLUR_OBJECTS;
  dsfieldmask->gpu_id = DEFAULT_GPU_ID;
  dsfieldmask->batch_size = DEFAULT_BATCH_SIZE;

  /* This quark is required to identify NvDsMeta when iterating through
   * the buffer metadatas */
  if (!_dsmeta_quark)
    _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);
}

/* Function called when a property of the element is set. Standard boilerplate.
 */
static void gst_dsfieldmask_set_property(GObject* object, guint prop_id, const GValue* value, GParamSpec* pspec) {
  GstDsFieldMask* dsfieldmask = GST_DSEXAMPLE(object);
  switch (prop_id) {
    case PROP_UNIQUE_ID:
      dsfieldmask->unique_id = g_value_get_uint(value);
      break;
    case PROP_PROCESSING_WIDTH:
      dsfieldmask->processing_width = g_value_get_int(value);
      break;
    case PROP_PROCESSING_HEIGHT:
      dsfieldmask->processing_height = g_value_get_int(value);
      break;
    case PROP_PROCESS_FULL_FRAME:
      dsfieldmask->process_full_frame = g_value_get_boolean(value);
      break;
    case PROP_BLUR_OBJECTS:
      dsfieldmask->blur_objects = g_value_get_boolean(value);
      break;
    case PROP_GPU_DEVICE_ID:
      dsfieldmask->gpu_id = g_value_get_uint(value);
      break;
    case PROP_BATCH_SIZE:
      dsfieldmask->batch_size = g_value_get_uint(value);
      break;
    case PROP_DETECTION_MASK_FILE:
      dsfieldmask->detection_mask_file = g_value_get_string(value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/* Function called when a property of the element is requested. Standard
 * boilerplate.
 */
static void gst_dsfieldmask_get_property(GObject* object, guint prop_id, GValue* value, GParamSpec* pspec) {
  GstDsFieldMask* dsfieldmask = GST_DSEXAMPLE(object);

  switch (prop_id) {
    case PROP_UNIQUE_ID:
      g_value_set_uint(value, dsfieldmask->unique_id);
      break;
    case PROP_PROCESSING_WIDTH:
      g_value_set_int(value, dsfieldmask->processing_width);
      break;
    case PROP_PROCESSING_HEIGHT:
      g_value_set_int(value, dsfieldmask->processing_height);
      break;
    case PROP_PROCESS_FULL_FRAME:
      g_value_set_boolean(value, dsfieldmask->process_full_frame);
      break;
    case PROP_BLUR_OBJECTS:
      g_value_set_boolean(value, dsfieldmask->blur_objects);
      break;
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint(value, dsfieldmask->gpu_id);
      break;
    case PROP_BATCH_SIZE:
      g_value_set_uint(value, dsfieldmask->batch_size);
      break;
    case PROP_DETECTION_MASK_FILE:
      g_value_set_string(value, dsfieldmask->detection_mask_file.c_str());
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
      break;
  }
}

/**
 * Initialize all resources and start the output thread
 */
static gboolean gst_dsfieldmask_start(GstBaseTransform* btrans) {
  GstDsFieldMask* dsfieldmask = GST_DSEXAMPLE(btrans);
  NvBufSurfaceCreateParams create_params = {0};

  DsFieldMaskInitParams init_params = {
      .processingWidth=dsfieldmask->processing_width,
      .processingHeight=dsfieldmask->processing_height,
      .fullFrame=dsfieldmask->process_full_frame,
      .detection_mask_file=dsfieldmask->detection_mask_file};

  int val = -1;

  /* Algorithm specific initializations and resource allocation. */
  dsfieldmask->dsfieldmasklib_ctx = DsFieldMaskCtxInit(&init_params);

  GST_DEBUG_OBJECT(dsfieldmask, "ctx lib %p \n", dsfieldmask->dsfieldmasklib_ctx);

  CHECK_CUDA_STATUS(cudaSetDevice(dsfieldmask->gpu_id), "Unable to set cuda device");

  cudaDeviceGetAttribute(&val, cudaDevAttrIntegrated, dsfieldmask->gpu_id);
  dsfieldmask->is_integrated = val;

  GST_DEBUG_OBJECT(dsfieldmask, "Setting batch-size %d \n", dsfieldmask->batch_size);

  if (dsfieldmask->process_full_frame && dsfieldmask->blur_objects) {
    GST_ERROR("Error: does not support blurring while processing full frame");
    goto error;
  }

#ifndef WITH_OPENCV
  if (dsfieldmask->blur_objects) {
    GST_ELEMENT_ERROR(
        dsfieldmask,
        STREAM,
        FAILED,
        ("OpenCV has been deprecated, hence object blurring will not work."
         "Enable OpenCV compilation in gst-dsfieldmask Makefile by setting 'WITH_OPENCV:=1"),
        (NULL));
    goto error;
  }
#endif

  CHECK_CUDA_STATUS(cudaStreamCreate(&dsfieldmask->cuda_stream), "Could not create cuda stream");

  if (dsfieldmask->inter_buf)
    NvBufSurfaceDestroy(dsfieldmask->inter_buf);
  dsfieldmask->inter_buf = NULL;

  /* An intermediate buffer for NV12/RGBA to BGR conversion  will be
   * required. Can be skipped if custom algorithm can work directly on
   * NV12/RGBA. */
  create_params.gpuId = dsfieldmask->gpu_id;
  create_params.width = dsfieldmask->processing_width;
  create_params.height = dsfieldmask->processing_height;
  create_params.size = 0;
  create_params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  create_params.layout = NVBUF_LAYOUT_PITCH;

  if (dsfieldmask->is_integrated) {
    create_params.memType = NVBUF_MEM_DEFAULT;
  } else {
    create_params.memType = NVBUF_MEM_CUDA_PINNED;
  }

  if (NvBufSurfaceCreate(&dsfieldmask->inter_buf, 1, &create_params) != 0) {
    GST_ERROR("Error: Could not allocate internal buffer for dsfieldmask");
    goto error;
  }

  /* Create host memory for storing converted/scaled interleaved RGB data */
  CHECK_CUDA_STATUS(
      cudaMallocHost(
          &dsfieldmask->host_rgb_buf, dsfieldmask->processing_width * dsfieldmask->processing_height * RGB_BYTES_PER_PIXEL),
      "Could not allocate cuda host buffer");

  GST_DEBUG_OBJECT(dsfieldmask, "allocated cuda buffer %p \n", dsfieldmask->host_rgb_buf);

#ifdef WITH_OPENCV
  /* CV Mat containing interleaved RGB data. This call does not allocate memory.
   * It uses host_rgb_buf as data. */
  dsfieldmask->cvmat = new cv::Mat(
      dsfieldmask->processing_height,
      dsfieldmask->processing_width,
      CV_8UC3,
      dsfieldmask->host_rgb_buf,
      dsfieldmask->processing_width * RGB_BYTES_PER_PIXEL);

  if (!dsfieldmask->cvmat)
    goto error;

  GST_DEBUG_OBJECT(dsfieldmask, "created CV Mat\n");
#endif

  /* Set the NvBufSurfTransform config parameters. */
  dsfieldmask->transform_config_params.compute_mode = NvBufSurfTransformCompute_Default;
  dsfieldmask->transform_config_params.gpu_id = dsfieldmask->gpu_id;

  return TRUE;
error:
  if (dsfieldmask->host_rgb_buf) {
    cudaFreeHost(dsfieldmask->host_rgb_buf);
    dsfieldmask->host_rgb_buf = NULL;
  }

  if (dsfieldmask->cuda_stream) {
    cudaStreamDestroy(dsfieldmask->cuda_stream);
    dsfieldmask->cuda_stream = NULL;
  }
  if (dsfieldmask->dsfieldmasklib_ctx)
    DsFieldMaskCtxDeinit(dsfieldmask->dsfieldmasklib_ctx);
  return FALSE;
}

/**
 * Stop the output thread and free up all the resources
 */
static gboolean gst_dsfieldmask_stop(GstBaseTransform* btrans) {
  GstDsFieldMask* dsfieldmask = GST_DSEXAMPLE(btrans);

  if (dsfieldmask->inter_buf)
    NvBufSurfaceDestroy(dsfieldmask->inter_buf);
  dsfieldmask->inter_buf = NULL;

  if (dsfieldmask->cuda_stream)
    cudaStreamDestroy(dsfieldmask->cuda_stream);
  dsfieldmask->cuda_stream = NULL;

#ifdef WITH_OPENCV
  delete dsfieldmask->cvmat;
  dsfieldmask->cvmat = NULL;
#endif

  if (dsfieldmask->host_rgb_buf) {
    cudaFreeHost(dsfieldmask->host_rgb_buf);
    dsfieldmask->host_rgb_buf = NULL;
  }

  GST_DEBUG_OBJECT(dsfieldmask, "deleted CV Mat \n");

  /* Deinit the algorithm library */
  DsFieldMaskCtxDeinit(dsfieldmask->dsfieldmasklib_ctx);
  dsfieldmask->dsfieldmasklib_ctx = NULL;

  GST_DEBUG_OBJECT(dsfieldmask, "ctx lib released \n");

  return TRUE;
}

/**
 * Called when source / sink pad capabilities have been negotiated.
 */
static gboolean gst_dsfieldmask_set_caps(GstBaseTransform* btrans, GstCaps* incaps, GstCaps* outcaps) {
  GstDsFieldMask* dsfieldmask = GST_DSEXAMPLE(btrans);
  /* Save the input video information, since this will be required later. */
  gst_video_info_from_caps(&dsfieldmask->video_info, incaps);

  if (dsfieldmask->blur_objects && !dsfieldmask->process_full_frame) {
    /* requires RGBA format for blurring the objects in opencv */
    if (dsfieldmask->video_info.finfo->format != GST_VIDEO_FORMAT_RGBA) {
      GST_ELEMENT_ERROR(
          dsfieldmask, STREAM, FAILED, ("input format should be RGBA when using blur-objects property"), (NULL));
      goto error;
    }
  }

  return TRUE;

error:
  return FALSE;
}

/**
 * Scale the entire frame to the processing resolution maintaining aspect ratio.
 * Or crop and scale objects to the processing resolution maintaining the aspect
 * ratio. Remove the padding required by hardware and convert from RGBA to RGB
 * using openCV. These steps can be skipped if the algorithm can work with
 * padded data and/or can work with RGBA.
 */
static GstFlowReturn get_converted_mat(
    GstDsFieldMask* dsfieldmask,
    NvBufSurface* input_buf,
    gint idx,
    NvOSD_RectParams* crop_rect_params,
    gdouble& ratio,
    gint input_width,
    gint input_height) {
  NvBufSurfTransform_Error err;
  NvBufSurfTransformConfigParams transform_config_params;
  NvBufSurfTransformParams transform_params;
  NvBufSurfTransformRect src_rect;
  NvBufSurfTransformRect dst_rect;
  NvBufSurface ip_surf;
#ifdef WITH_OPENCV
  cv::Mat in_mat;
#endif
  ip_surf = *input_buf;

  ip_surf.numFilled = ip_surf.batchSize = 1;
  ip_surf.surfaceList = &(input_buf->surfaceList[idx]);

  gint src_left = GST_ROUND_UP_2((unsigned int)crop_rect_params->left);
  gint src_top = GST_ROUND_UP_2((unsigned int)crop_rect_params->top);
  gint src_width = GST_ROUND_DOWN_2((unsigned int)crop_rect_params->width);
  gint src_height = GST_ROUND_DOWN_2((unsigned int)crop_rect_params->height);

  /* Maintain aspect ratio */
  double hdest = dsfieldmask->processing_width * src_height / (double)src_width;
  double wdest = dsfieldmask->processing_height * src_width / (double)src_height;
  guint dest_width, dest_height;

  if (hdest <= dsfieldmask->processing_height) {
    dest_width = dsfieldmask->processing_width;
    dest_height = hdest;
  } else {
    dest_width = wdest;
    dest_height = dsfieldmask->processing_height;
  }

  /* Configure transform session parameters for the transformation */
  transform_config_params.compute_mode = dsfieldmask->transform_config_params.compute_mode;
  transform_config_params.gpu_id = dsfieldmask->gpu_id;
  transform_config_params.cuda_stream = dsfieldmask->cuda_stream;

  /* Set the transform session parameters for the conversions executed in this
   * thread. */
  err = NvBufSurfTransformSetSessionParams(&transform_config_params);
  if (err != NvBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR(
        dsfieldmask, STREAM, FAILED, ("NvBufSurfTransformSetSessionParams failed with error %d", err), (NULL));
    goto error;
  }

  /* Calculate scaling ratio while maintaining aspect ratio */
  ratio = MIN(1.0 * dest_width / src_width, 1.0 * dest_height / src_height);

  if ((crop_rect_params->width == 0) || (crop_rect_params->height == 0)) {
    GST_ELEMENT_ERROR(dsfieldmask, STREAM, FAILED, ("%s:crop_rect_params dimensions are zero", __func__), (NULL));
    goto error;
  }

#ifdef __aarch64__
  if (ratio <= 1.0 / 16 || ratio >= 16.0) {
    /* Currently cannot scale by ratio > 16 or < 1/16 for Jetson */
    goto error;
  }
#endif
  /* Set the transform ROIs for source and destination */
  src_rect = {(guint)src_top, (guint)src_left, (guint)src_width, (guint)src_height};
  dst_rect = {0, 0, (guint)dest_width, (guint)dest_height};

  /* Set the transform parameters */
  transform_params.src_rect = &src_rect;
  transform_params.dst_rect = &dst_rect;
  transform_params.transform_flag =
      NVBUFSURF_TRANSFORM_FILTER | NVBUFSURF_TRANSFORM_CROP_SRC | NVBUFSURF_TRANSFORM_CROP_DST;
  transform_params.transform_filter = NvBufSurfTransformInter_Default;

  /* Memset the memory */
  NvBufSurfaceMemSet(dsfieldmask->inter_buf, 0, 0, 0);

  GST_DEBUG_OBJECT(dsfieldmask, "Scaling and converting input buffer\n");

  /* Transformation scaling+format conversion if any. */
  err = NvBufSurfTransform(&ip_surf, dsfieldmask->inter_buf, &transform_params);
  if (err != NvBufSurfTransformError_Success) {
    GST_ELEMENT_ERROR(
        dsfieldmask, STREAM, FAILED, ("NvBufSurfTransform failed with error %d while converting buffer", err), (NULL));
    goto error;
  }
  /* Map the buffer so that it can be accessed by CPU */
  if (NvBufSurfaceMap(dsfieldmask->inter_buf, 0, 0, NVBUF_MAP_READ) != 0) {
    goto error;
  }
  if (dsfieldmask->inter_buf->memType == NVBUF_MEM_SURFACE_ARRAY) {
    /* Cache the mapped data for CPU access */
    NvBufSurfaceSyncForCpu(dsfieldmask->inter_buf, 0, 0);
  }

#ifdef WITH_OPENCV
  /* Use openCV to remove padding and convert RGBA to BGR. Can be skipped if
   * algorithm can handle padded RGBA data. */
  in_mat = cv::Mat(
      dsfieldmask->processing_height,
      dsfieldmask->processing_width,
      CV_8UC4,
      dsfieldmask->inter_buf->surfaceList[0].mappedAddr.addr[0],
      dsfieldmask->inter_buf->surfaceList[0].pitch);

#if (CV_MAJOR_VERSION >= 4)
  cv::cvtColor(in_mat, *dsfieldmask->cvmat, cv::COLOR_RGBA2BGR);
#else
  cv::cvtColor(in_mat, *dsfieldmask->cvmat, CV_RGBA2BGR);
#endif
#endif

  if (NvBufSurfaceUnMap(dsfieldmask->inter_buf, 0, 0)) {
    goto error;
  }

  if (dsfieldmask->is_integrated) {
#ifdef __aarch64__
    /* To use the converted buffer in CUDA, create an EGLImage and then use
     * CUDA-EGL interop APIs */
    if (USE_EGLIMAGE) {
      if (NvBufSurfaceMapEglImage(dsfieldmask->inter_buf, 0) != 0) {
        goto error;
      }

      /* dsfieldmask->inter_buf->surfaceList[0].mappedAddr.eglImage
       * Use interop APIs cuGraphicsEGLRegisterImage and
       * cuGraphicsResourceGetMappedEglFrame to access the buffer in CUDA */

      /* Destroy the EGLImage */
      NvBufSurfaceUnMapEglImage(dsfieldmask->inter_buf, 0);
    }
#endif
  }

  /* We will first convert only the Region of Interest (the entire frame or the
   * object bounding box) to RGB and then scale the converted RGB frame to
   * processing resolution. */
  return GST_FLOW_OK;

error:
  return GST_FLOW_ERROR;
}

/**
 * Called when element recieves an input buffer from upstream element.
 */
static GstFlowReturn gst_dsfieldmask_transform_ip(GstBaseTransform* btrans, GstBuffer* inbuf) {
  GstDsFieldMask* dsfieldmask = GST_DSEXAMPLE(btrans);
  GstMapInfo in_map_info;
  GstFlowReturn flow_ret = GST_FLOW_ERROR;
  gdouble scale_ratio = 1.0;
  DsFieldMaskOutput* output{nullptr};

  NvBufSurface* surface = NULL;
  NvDsBatchMeta* batch_meta = NULL;
  NvDsFrameMeta* frame_meta = NULL;
  NvDsMetaList* l_frame = NULL;
  guint i = 0;

  dsfieldmask->frame_num++;
  CHECK_CUDA_STATUS(cudaSetDevice(dsfieldmask->gpu_id), "Unable to set cuda device");

  memset(&in_map_info, 0, sizeof(in_map_info));
  if (!gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
    g_print("Error: Failed to map gst buffer\n");
    goto error;
  }

  nvds_set_input_system_timestamp(inbuf, GST_ELEMENT_NAME(dsfieldmask));
  surface = (NvBufSurface*)in_map_info.data;
  GST_DEBUG_OBJECT(dsfieldmask, "Processing Frame %" G_GUINT64_FORMAT " Surface %p\n", dsfieldmask->frame_num, surface);

  if (CHECK_NVDS_MEMORY_AND_GPUID(dsfieldmask, surface))
    goto error;

  batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  if (batch_meta == nullptr) {
    GST_ELEMENT_ERROR(dsfieldmask, STREAM, FAILED, ("NvDsBatchMeta not found for input buffer."), (NULL));
    return GST_FLOW_ERROR;
  }

  if (dsfieldmask->process_full_frame) {
    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
      frame_meta = (NvDsFrameMeta*)(l_frame->data);
      NvOSD_RectParams rect_params;

      /* Scale the entire frame to processing resolution */
      rect_params.left = 0;
      rect_params.top = 0;
      rect_params.width = dsfieldmask->video_info.width;
      rect_params.height = dsfieldmask->video_info.height;

      /* Scale and convert the frame */
      if (get_converted_mat(
              dsfieldmask,
              surface,
              i,
              &rect_params,
              scale_ratio,
              dsfieldmask->video_info.width,
              dsfieldmask->video_info.height) != GST_FLOW_OK) {
        goto error;
      }

      /* Process to get the output */
#ifdef WITH_OPENCV
      output = DsFieldMaskProcess(frame_meta, dsfieldmask->dsfieldmasklib_ctx, dsfieldmask->cvmat->data);
#else
      output = DsFieldMaskProcess(
          dsfieldmask->dsfieldmasklib_ctx,
          (unsigned char*)dsfieldmask->inter_buf->surfaceList[0].mappedAddr.addr[0]);
#endif
      i++;
      free(output);
    }

  } else {
    /* Using object crops as input to the algorithm. The objects are detected by
     * the primary detector */
    NvDsMetaList* l_obj = NULL;
    NvDsObjectMeta* obj_meta = NULL;

    if (!dsfieldmask->is_integrated) {
      if (dsfieldmask->blur_objects) {
        if (!(surface->memType == NVBUF_MEM_CUDA_UNIFIED || surface->memType == NVBUF_MEM_CUDA_PINNED)) {
          GST_ELEMENT_ERROR(
              dsfieldmask,
              STREAM,
              FAILED,
              ("%s:need NVBUF_MEM_CUDA_UNIFIED or NVBUF_MEM_CUDA_PINNED memory for opencv blurring", __func__),
              (NULL));
          return GST_FLOW_ERROR;
        }
      }
    }
    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
      frame_meta = (NvDsFrameMeta*)(l_frame->data);

#ifdef WITH_OPENCV
      cv::Mat in_mat;

      if (dsfieldmask->blur_objects) {
        /* Map the buffer so that it can be accessed by CPU */
        if (surface->surfaceList[frame_meta->batch_id].mappedAddr.addr[0] == NULL) {
          if (NvBufSurfaceMap(surface, frame_meta->batch_id, 0, NVBUF_MAP_READ_WRITE) != 0) {
            GST_ELEMENT_ERROR(
                dsfieldmask, STREAM, FAILED, ("%s:buffer map to be accessed by CPU failed", __func__), (NULL));
            return GST_FLOW_ERROR;
          }
        }

        /* Cache the mapped data for CPU access */
        if (dsfieldmask->inter_buf->memType == NVBUF_MEM_SURFACE_ARRAY)
          NvBufSurfaceSyncForCpu(surface, frame_meta->batch_id, 0);

        in_mat = cv::Mat(
            surface->surfaceList[frame_meta->batch_id].planeParams.height[0],
            surface->surfaceList[frame_meta->batch_id].planeParams.width[0],
            CV_8UC4,
            surface->surfaceList[frame_meta->batch_id].mappedAddr.addr[0],
            surface->surfaceList[frame_meta->batch_id].planeParams.pitch[0]);
      }
#endif

      DsFieldMaskProcessFrame(frame_meta, dsfieldmask->dsfieldmasklib_ctx);
#if 0
      for (l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
        obj_meta = (NvDsObjectMeta*)(l_obj->data);

        if (dsfieldmask->blur_objects) {
          /* gaussian blur the detected objects using opencv */
#ifdef WITH_OPENCV
          if (blur_objects(dsfieldmask, frame_meta->batch_id, &obj_meta->rect_params, in_mat) != GST_FLOW_OK) {
            /* Error in blurring, skip processing on object. */
            GST_ELEMENT_ERROR(dsfieldmask, STREAM, FAILED, ("blurring the object failed"), (NULL));
            if (NvBufSurfaceUnMap(surface, frame_meta->batch_id, 0)) {
              GST_ELEMENT_ERROR(
                  dsfieldmask, STREAM, FAILED, ("%s:buffer unmap to be accessed by CPU failed", __func__), (NULL));
            }
            return GST_FLOW_ERROR;
          }
          continue;
#else
          GST_ELEMENT_ERROR(
              dsfieldmask,
              STREAM,
              FAILED,
              ("OpenCV has been deprecated, hence object blurring will not work."
               "Enable OpenCV compilation in gst-dsfieldmask Makefile by setting 'WITH_OPENCV:=1"),
              (NULL));
          return GST_FLOW_ERROR;
#endif
        }

        // gout << "Object: " << "l=" << obj_meta->rect_params.left
        //           << ", t=" << obj_meta->rect_params.top
        //           << ", w=" << obj_meta->rect_params.width
        //           << ", h=" << obj_meta->rect_params.height << std::endl;

        /* Should not process on objects smaller than MIN_INPUT_OBJECT_WIDTH x
         * MIN_INPUT_OBJECT_HEIGHT */
        if (obj_meta->rect_params.width < MIN_INPUT_OBJECT_WIDTH ||
            obj_meta->rect_params.height < MIN_INPUT_OBJECT_HEIGHT)
          continue;

        /* Extra check for Jetson devices as default compute mode on Jetson is
         * VIC which supports min 16x16 */
        if (dsfieldmask->is_integrated) {
          if (dsfieldmask->transform_config_params.compute_mode == NvBufSurfTransformCompute_VIC ||
              dsfieldmask->transform_config_params.compute_mode == NvBufSurfTransformCompute_Default) {
            if (obj_meta->rect_params.width < 16 || obj_meta->rect_params.height < 16)
              continue;
          }
        }

#if 0
        /* Crop and scale the object */
        if (get_converted_mat(
                dsfieldmask,
                surface,
                frame_meta->batch_id,
                &obj_meta->rect_params,
                scale_ratio,
                dsfieldmask->video_info.width,
                dsfieldmask->video_info.height) != GST_FLOW_OK) {
          /* Error in conversion, skip processing on object. */
          continue;
        }
#endif

#if 1

#ifdef WITH_OPENCV
        /* Process the object crop to obtain label */
        output = DsFieldMaskProcess(dsfieldmask->dsfieldmasklib_ctx, dsfieldmask->cvmat->data);
#else
        /* Process the object crop to obtain label */
        output = DsFieldMaskProcess(
            dsfieldmask->dsfieldmasklib_ctx,
            (unsigned char*)dsfieldmask->inter_buf->surfaceList[0].mappedAddr.addr[0]);
#endif

        /* Attach labels for the object */
        attach_metadata_object(dsfieldmask, obj_meta, output);

        free(output);
#endif
      }
#endif
      if (dsfieldmask->blur_objects) {
        /* Cache the mapped data for device access */
        if (dsfieldmask->inter_buf->memType == NVBUF_MEM_SURFACE_ARRAY)
          NvBufSurfaceSyncForDevice(surface, frame_meta->batch_id, 0);

#ifdef WITH_OPENCV
#ifdef DSEXAMPLE_DEBUG
          /* Use openCV to remove padding and convert RGBA to BGR. Can be
           * skipped if algorithm can handle padded RGBA data. */
#if (CV_MAJOR_VERSION >= 4)
        cv::cvtColor(in_mat, *dsfieldmask->cvmat, cv::COLOR_RGBA2BGR);
#else
        cv::cvtColor(in_mat, *dsfieldmask->cvmat, CV_RGBA2BGR);
#endif
        /* used to dump the converted mat to files for debug */
        static guint cnt = 0;
        cv::imwrite("out_" + std::to_string(cnt) + ".jpeg", *dsfieldmask->cvmat);
        cnt++;
#endif
#endif
      }
    }
  }
  flow_ret = GST_FLOW_OK;

error:

  nvds_set_output_system_timestamp(inbuf, GST_ELEMENT_NAME(dsfieldmask));
  gst_buffer_unmap(inbuf, &in_map_info);
  return flow_ret;
}

/**
 * Boiler plate for registering a plugin and an element.
 */
static gboolean dsfieldmask_plugin_init(GstPlugin* plugin) {
  GST_DEBUG_CATEGORY_INIT(gst_dsfieldmask_debug, "dsfieldmask", 0, "dsfieldmask plugin");

  return gst_element_register(plugin, "dsfieldmask", GST_RANK_PRIMARY, GST_TYPE_DSEXAMPLE);
}
}

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvdsgst_dsfieldmask,
    DESCRIPTION,
    dsfieldmask_plugin_init,
    "7.1",
    LICENSE,
    BINARY_PACKAGE,
    URL)
