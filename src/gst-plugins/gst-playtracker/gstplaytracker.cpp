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

#include "gstplaytracker.h"
#include "gstnvdsmeta.h"

#include "hstream/src/libs/common/utils.h"

#include <sys/time.h>
#include <cassert>

inline bool CUDA_CHECK_(gint e, gint iLine, const gchar* szFile) {
  if (e != cudaSuccess) {
    std::cout << "CUDA runtime error " << e << " at line " << iLine << " in file " << szFile << std::endl;
    return false;
  }
  return true;
}

#define cuda_ck(call) CUDA_CHECK_(call, __LINE__, __FILE__)

#define cuda_ck(call) CUDA_CHECK_(call, __LINE__, __FILE__)

GST_DEBUG_CATEGORY_STATIC(gst_playtracker_debug);

static GQuark _dsmeta_quark = 0;

/* Enum to identify properties */
enum {
  PROP_0,
  PROP_UNIQUE_ID,
  PROP_DRAW,
  PROP_GPU_DEVICE_ID,
  PROP_PLAY_TRACKER_CONFIG_FILE,
};

/* Default values for properties */
#define DEFAULT_UNIQUE_ID 15
#define DEFAULT_GPU_ID 0

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

static GstFlowReturn gst_playtracker_transform(GstBaseTransform* btrans, GstBuffer* inbuf);

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

  gstbasetransform_class->transform_ip = GST_DEBUG_FUNCPTR(gst_playtracker_transform);

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
  playtracker->gpu_id = DEFAULT_GPU_ID;
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
    case PROP_GPU_DEVICE_ID:
      playtracker->gpu_id = g_value_get_uint(value);
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
    case PROP_GPU_DEVICE_ID:
      g_value_set_uint(value, playtracker->gpu_id);
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
  DsPlayTrackerInitParams init_params = {
      .play_tracker_config_file = playtracker->play_tracker_config_file,
      .draw = !!playtracker->draw,
  };

  /* Algorithm specific initializations and resource allocation. */
  playtracker->playtrackerlib_ctx = DsPlayTrackerCtxInit(&init_params);

  GST_DEBUG_OBJECT(playtracker, "ctx lib %p \n", playtracker->playtrackerlib_ctx);

  cudaError_t CUerr = cudaSetDevice(playtracker->gpu_id);
  if (CUerr != cudaSuccess) {
    GST_ERROR_OBJECT(playtracker, "cudaSetDevice Failed in %s\n", __func__);
    return FALSE;
  }
  cuda_ck(cudaStreamCreate(&(playtracker->stream)));

  /* Create process queue and cvmat queue to transfer data between threads.
   * We will be using this queue to maintain the list of frames/objects
   * currently given to the algorithm for processing. */
  playtracker->process_queue = g_queue_new();
  playtracker->buf_queue = g_queue_new();

  for (int i = 0; i < 2; i++) {
    // g_queue_push_tail(playtracker->buf_queue, inter_buf);
    // FAKE BUFFER
    g_queue_push_tail(playtracker->buf_queue, nullptr);
  }

  return TRUE;
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
    if (inter_buf) {
      assert(false); // why we owning any surfaces? see about removing all of this crap.
      NvBufSurfaceDestroy(inter_buf);
    }
    inter_buf = NULL;
  }
  playtracker->stop = TRUE;

  g_cond_broadcast(&playtracker->process_cond);
  g_mutex_unlock(&playtracker->process_lock);

  if (playtracker->process_thread) {
    g_thread_join(playtracker->process_thread);
    playtracker->process_thread = nullptr;
  }

  // Deinit the algorithm library
  DsPlayTrackerCtxDeinit(playtracker->playtrackerlib_ctx);
  playtracker->playtrackerlib_ctx = NULL;

  GST_DEBUG_OBJECT(playtracker, "ctx lib released \n");

  g_queue_free(playtracker->process_queue);

  g_queue_free(playtracker->buf_queue);

  if (playtracker->stream) {
    cudaStreamDestroy(playtracker->stream);
    playtracker->stream = nullptr;
  }

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
 * Attach metadata for the full frame. We will be adding a new metadata.
 */
static void attach_metadata_full_frame(
    GstDsPlayTracker* playtracker,
    NvDsFrameMeta* frame_meta,
    const hm::play_tracker::PlayTrackerResults& play_results,
    guint batch_id) {
  NvDsBatchMeta* batch_meta = frame_meta->base_meta.batch_meta;
  NvDsObjectMeta* object_meta = NULL;

  size_t adder = 0;
  for (int64_t i = play_results.tracking_boxes.size() - 1; i >= 0; --i, ++adder) {
    const hm::BBox& tracking_box = play_results.tracking_boxes[i];
    object_meta = nvds_acquire_obj_meta_from_pool(batch_meta);
    object_meta->class_id = DsPlayTrackerInitParams::kPlayBoxClassIdBase + adder;

    NvOSD_RectParams& rect_params = object_meta->rect_params;

    // Assign bounding box coordinates
    rect_params.left = tracking_box.left;
    rect_params.top = tracking_box.top;
    rect_params.width = tracking_box.width();
    rect_params.height = tracking_box.height();

    rect_params.border_width = 0;
    rect_params.border_color = (NvOSD_ColorParams){1, 1, 0, 1};

    object_meta->object_id = UNTRACKED_OBJECT_ID;

    nvds_add_obj_meta_to_frame(frame_meta, object_meta, NULL);
  }
}

static GstFlowReturn gst_playtracker_transform(GstBaseTransform* btrans, GstBuffer* inbuf) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  NvBufSurface* in_surface = NULL;

  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  assert(batch_meta);

  if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ))
    goto invalid_inbuf;

  in_surface = (NvBufSurface*)inmap.data;

  GST_DEBUG_OBJECT(playtracker, "transform");

  for (guint i = 0; i < batch_meta->num_frames_in_batch; i++) {
    GstDsPlayTrackerFrame frame;
    frame.obj_meta = nullptr;
    frame.frame_meta = nvds_get_nth_frame_meta(batch_meta->frame_meta_list, i);
    frame.frame_num = frame.frame_meta->frame_num;
    frame.batch_index = i;
    frame.input_surf_params = in_surface->surfaceList + i;
    if (DsPlayTrackerProcessFrame(playtracker->playtrackerlib_ctx, frame, playtracker->stream)) {
      attach_metadata_full_frame(playtracker, frame.frame_meta, frame.play_tracker_results, frame.batch_index);
    }
  }

  gst_buffer_unmap(inbuf, &inmap);

  return GST_FLOW_OK;

invalid_inbuf: {
  GST_ERROR("input buffer mapinfo failed");
  return GST_FLOW_ERROR;
}
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
