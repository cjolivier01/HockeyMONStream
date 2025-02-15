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

#include "src/libs/common/utils.h"

#include <sys/time.h>
#include <cassert>
#include <memory>

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
// #define GST_CAT_DEFAULT gst_playtracker_debug

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

// static GstFlowReturn gst_playtracker_submit_input_buffer(GstBaseTransform* btrans, gboolean discont, GstBuffer*
// inbuf); static GstFlowReturn gst_playtracker_generate_output(GstBaseTransform* btrans, GstBuffer** outbuf);

static gpointer gst_playtracker_output_loop(gpointer data);

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

  // gstbasetransform_class->submit_input_buffer = GST_DEBUG_FUNCPTR(gst_playtracker_submit_input_buffer);
  // gstbasetransform_class->generate_output = GST_DEBUG_FUNCPTR(gst_playtracker_generate_output);

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
  // std::string nvtx_str;
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

  /* Start a thread which will pop output from the algorithm, form NvDsMeta and
   * push buffers to the next element. */
  // playtracker->process_thread = g_thread_new("playtracker-process-thread", gst_playtracker_output_loop, playtracker);

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
    if (inter_buf)
      NvBufSurfaceDestroy(inter_buf);
    inter_buf = NULL;
  }
  playtracker->stop = TRUE;

  g_cond_broadcast(&playtracker->process_cond);
  g_mutex_unlock(&playtracker->process_lock);

  g_thread_join(playtracker->process_thread);

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

// static gboolean convert_batch_and_push_to_process_thread(GstDsPlayTracker* playtracker, GstDsPlayTrackerBatch* batch)
// {
//   g_mutex_lock(&playtracker->process_lock);

//   /* Wait if buf queue is empty. */
//   while (g_queue_is_empty(playtracker->buf_queue)) {
//     g_cond_wait(&playtracker->buf_cond, &playtracker->process_lock);
//   }

//   /* Pop a buffer from the element's buf queue. */
//   batch->inter_buf = (NvBufSurface*)g_queue_pop_head(playtracker->buf_queue);
//   assert(!batch->inter_buf); // always null now, just for timing I guess

//   g_mutex_unlock(&playtracker->process_lock);

//   //
//   // Used to do something here?
//   //

//   /* Push the batch info structure in the processing queue and notify the
//    * process thread that a new batch has been queued. */
//   g_mutex_lock(&playtracker->process_lock);

//   g_queue_push_tail(playtracker->process_queue, batch);
//   g_cond_broadcast(&playtracker->process_cond);

//   g_mutex_unlock(&playtracker->process_lock);

//   return TRUE;
// }

/**
 * Called when element recieves an input buffer from upstream element.
 */
// static GstFlowReturn gst_playtracker_submit_input_buffer(GstBaseTransform* btrans, gboolean discont, GstBuffer*
// inbuf) {
//   GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
//   GstMapInfo in_map_info;
//   NvBufSurface* in_surf{nullptr};
//   GstDsPlayTrackerBatch* buf_push_batch{nullptr};
//   std::unique_ptr<GstDsPlayTrackerBatch> batch = nullptr;

//   NvDsBatchMeta* batch_meta = NULL;
//   // gdouble scale_ratio = 1.0;
//   guint num_filled = 0;

//   struct cudaDeviceProp prop;
//   cudaGetDeviceProperties(&prop, playtracker->gpu_id);

//   playtracker->current_batch_num++;

//   memset(&in_map_info, 0, sizeof(in_map_info));

//   /* Map the buffer contents and get the pointer to NvBufSurface. */
//   if (!gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
//     GST_ELEMENT_ERROR(
//         playtracker, STREAM, FAILED, ("%s:gst buffer map to get pointer to NvBufSurface failed", __func__), (NULL));
//     return GST_FLOW_ERROR;
//   }
//   in_surf = (NvBufSurface*)in_map_info.data;

//   nvds_set_input_system_timestamp(inbuf, GST_ELEMENT_NAME(playtracker));

//   batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
//   if (batch_meta == nullptr) {
//     GST_ELEMENT_ERROR(playtracker, STREAM, FAILED, ("NvDsBatchMeta not found for input buffer."), (NULL));
//     return GST_FLOW_ERROR;
//   }
//   num_filled = batch_meta->num_frames_in_batch;

//   for (guint i = 0; i < num_filled; i++) {
//     if (batch == nullptr) {
//       batch.reset(new GstDsPlayTrackerBatch);
//       batch->push_buffer = FALSE;
//       batch->inbuf = inbuf;
//       batch->inbuf_batch_num = playtracker->current_batch_num;
//     }

//     /* Adding a frame to the current batch. Set the frames members. */
//     GstDsPlayTrackerFrame frame;
//     // frame.scale_ratio_x = scale_ratio;
//     // frame.scale_ratio_y = scale_ratio;
//     frame.obj_meta = nullptr;
//     frame.frame_meta = nvds_get_nth_frame_meta(batch_meta->frame_meta_list, i);
//     frame.frame_num = frame.frame_meta->frame_num;
//     frame.batch_index = i;
//     frame.input_surf_params = in_surf->surfaceList + i;
//     batch->frames.push_back(frame);

//     // Set the transform session parameters for the conversions executed in
//     // this thread.
//     if (i == num_filled) {
//       if (!convert_batch_and_push_to_process_thread(playtracker, batch.get())) {
//         return GST_FLOW_ERROR;
//       }
//       /* Batch submitted. Set batch to nullptr so that a new GstDsPlayTrackerBatch
//        * structure can be allocated if required. */
//       (void)batch.release();
//     }
//   }
//   /* Submit a non-full batch. */
//   if (batch) {
//     if (!convert_batch_and_push_to_process_thread(playtracker, batch.get())) {
//       return GST_FLOW_ERROR;
//     }
//     (void)batch.release();
//   }

//   /* Queue a push buffer batch. This batch is not inferred. This batch is to
//    * signal the process thread that there are no more batches
//    * belonging to this input buffer and this GstBuffer can be pushed to
//    * downstream element once all the previous processing is done. */
//   buf_push_batch = new GstDsPlayTrackerBatch;
//   buf_push_batch->inbuf = inbuf;
//   buf_push_batch->push_buffer = TRUE;

//   g_mutex_lock(&playtracker->process_lock);
//   /* Check if this is a push buffer or event marker batch. If yes, no need to
//    * queue the input for inferencing. */
//   if (buf_push_batch->push_buffer) {
//     /* Push the batch info structure in the processing queue and notify the
//      * process thread that a new batch has been queued. */
//     g_queue_push_tail(playtracker->process_queue, buf_push_batch);
//     g_cond_broadcast(&playtracker->process_cond);
//   }
//   g_mutex_unlock(&playtracker->process_lock);

//   return GST_FLOW_OK;
// }

/**
 * If submit_input_buffer is implemented, it is mandatory to implement
 * generate_output. Buffers are not pushed to the downstream element from here.
 * Return the GstFlowReturn value of the latest pad push so that any error might
 * be caught by the application.
 */
// static GstFlowReturn gst_playtracker_generate_output(GstBaseTransform* btrans, GstBuffer** outbuf) {
//   GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
//   return playtracker->last_flow_ret;
// }

/**
 * Attach metadata for the full frame. We will be adding a new metadata.
 */
static void attach_metadata_full_frame(
    GstDsPlayTracker* playtracker,
    NvDsFrameMeta* frame_meta,
    const hm::play_tracker::PlayTrackerResults& play_results,
    guint batch_id) {
  // if (play_results.tracking_boxes.back().empty()) {
  //   return;
  // }
  NvDsBatchMeta* batch_meta = frame_meta->base_meta.batch_meta;
  NvDsObjectMeta* object_meta = NULL;

  // static gchar font_name[] = "Serif";
  size_t adder = 0;
  for (int64_t i = play_results.tracking_boxes.size() - 1; i >= 0; --i, ++adder) {
    const hm::BBox& tracking_box = play_results.tracking_boxes[i];
    object_meta = nvds_acquire_obj_meta_from_pool(batch_meta);
    object_meta->class_id = DsPlayTrackerInitParams::kPlayBoxClassIdBase + adder;

    NvOSD_RectParams& rect_params = object_meta->rect_params;
    // NvOSD_TextParams& text_params = object_meta->text_params;

    // Assign bounding box coordinates
    rect_params.left = tracking_box.left;
    rect_params.top = tracking_box.top;
    rect_params.width = tracking_box.width();
    rect_params.height = tracking_box.height();

    // Semi-transparent yellow background
    // rect_params.has_bg_color = 1;
    // rect_params.bg_color = (NvOSD_ColorParams){1, 1, 0, 0.1};
    // Red border of width 6
    rect_params.border_width = 0;
    rect_params.border_color = (NvOSD_ColorParams){1, 1, 0, 1};

    object_meta->object_id = UNTRACKED_OBJECT_ID;
    // g_strlcpy(object_meta->obj_label, obj->label, MAX_LABEL_SIZE);
    // // display_text required heap allocated memory
    // text_params.display_text = g_strdup(obj->label);
    // // Display text above the left top corner of the object
    // text_params.x_offset = rect_params.left;
    // text_params.y_offset = rect_params.top - 10;
    // // Set black background for the text
    // text_params.set_bg_clr = 1;
    // text_params.text_bg_clr = (NvOSD_ColorParams){0, 0, 0, 1};
    // // Font face, size and color
    // text_params.font_params.font_name = font_name;
    // text_params.font_params.font_size = 11;
    // text_params.font_params.font_color = (NvOSD_ColorParams){1, 1, 1, 1};

    nvds_add_obj_meta_to_frame(frame_meta, object_meta, NULL);
  }
}

static GstFlowReturn gst_playtracker_transform(GstBaseTransform* btrans, GstBuffer* inbuf) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(btrans);
  GstMapInfo inmap = GST_MAP_INFO_INIT;
  GstMapInfo outmap = GST_MAP_INFO_INIT;
  NvBufSurface* in_surface = NULL;
  NvBufSurface* out_surface = NULL;
  cudaError cudaErr = cudaSuccess;
  gchar pts_str[64];

  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(inbuf);
  assert(batch_meta);

  // videoprep->frame_num++;
  // GST_DEBUG_OBJECT(videoprep, "%s : Frame=%d InBuf=%p OutBuf=%p\n", __func__, videoprep->frame_num, inbuf, outbuf);

  if (!gst_buffer_map(inbuf, &inmap, GST_MAP_READ))
    goto invalid_inbuf;

  // if (!gst_buffer_map(outbuf, &outmap, GST_MAP_WRITE))
  //   goto invalid_outbuf;

  GST_DEBUG_OBJECT(playtracker, "transform");
  // if (videoprep->input_feature == MEM_FEATURE_NVMM) {
  //   in_surface = (NvBufSurface*)inmap.data;
  //   // TODO:
  //   if (CHECK_NVDS_MEMORY_AND_GPUID(videoprep, in_surface)) {
  //     gst_buffer_unmap(inbuf, &inmap);
  //     gst_buffer_unmap(outbuf, &outmap);
  //     return GST_FLOW_ERROR;
  //   }
  // }

  // if (videoprep->output_feature == MEM_FEATURE_NVMM) {
  //   out_surface = (NvBufSurface*)outmap.data;
  //   if (CHECK_NVDS_MEMORY_AND_GPUID(videoprep, out_surface)) {
  //     gst_buffer_unmap(inbuf, &inmap);
  //     gst_buffer_unmap(outbuf, &outmap);
  //     return GST_FLOW_ERROR;
  //   }
  // }

  // START_PROFILE;
  // videoprep->out_gst_buf = outbuf;
  // cudaErr = gst_videoprep_do_prep(batch_meta, videoprep, in_surface, out_surface);
  // if (cudaErr != cudaSuccess) {
  //   GST_ERROR_OBJECT(videoprep, "gst_videoprep_do_prep failed");
  //   return GST_FLOW_ERROR;
  // }
  // STOP_PROFILE("********* TOTAL DEWARP AND SCALE TIME *********");

  // GST_BUFFER_PTS(outbuf) = GST_BUFFER_PTS(inbuf);

  // GST_INFO_OBJECT(
  //     videoprep,
  //     " : source_id %d Frame=%d OUT-BUFFER %s",
  //     videoprep->source_id,
  //     videoprep->frame_num,
  //     print_pretty_time(pts_str, sizeof(pts_str), GST_BUFFER_PTS(outbuf)));

  gst_buffer_unmap(inbuf, &inmap);
  //gst_buffer_unmap(outbuf, &outmap);

  // if (!gst_buffer_copy_into(outbuf, inbuf, (GstBufferCopyFlags)GST_BUFFER_COPY_METADATA, 0, -1)) {
  //   GST_DEBUG_OBJECT(playtracker, "Buffer metadata copy failed \n");
  // }

  // if (videoprep->deref_input_buffer) {
  //   gst_buffer_unref(inbuf);
  // }

  return GST_FLOW_OK;

invalid_inbuf: {
  GST_ERROR("input buffer mapinfo failed");
  return GST_FLOW_ERROR;
}

invalid_outbuf: {
  GST_ERROR_OBJECT(playtracker, "output buffer mapinfo failed");
  gst_buffer_unmap(inbuf, &inmap);
  return GST_FLOW_ERROR;
}
}

/**
 * Output loop used to pop output from processing thread, attach the output to
 * the buffer in form of NvDsMeta and push the buffer to downstream element.
 */
static gpointer gst_playtracker_output_loop(gpointer data) {
  GstDsPlayTracker* playtracker = GST_DSPLAYTRACKER(data);

  struct cudaDeviceProp prop;
  cudaGetDeviceProperties(&prop, playtracker->gpu_id);

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

    /* For each frame attach metadata output. */
    for (guint i = 0; i < batch->frames.size(); i++) {
      GstDsPlayTrackerFrame& frame = batch->frames[i];
      if (DsPlayTrackerProcessFrame(frame, playtracker->playtrackerlib_ctx, playtracker->stream)) {
        attach_metadata_full_frame(playtracker, frame.frame_meta, frame.play_tracker_results, frame.batch_index);
      }
    }

    g_mutex_lock(&playtracker->process_lock);

    g_queue_push_tail(playtracker->buf_queue, batch->inter_buf);
    g_cond_broadcast(&playtracker->buf_cond);
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
