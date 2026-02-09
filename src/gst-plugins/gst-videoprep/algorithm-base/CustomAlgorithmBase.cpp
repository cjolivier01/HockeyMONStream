#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/CustomAlgorithmBase.h"
#include "hstream/src/libs/common/ApplicationPayload.h"
#include "hstream/src/libs/common/Status.h"

#ifdef HAS_NVDS_CUSTOMUSERMETA
#include <nvdsdummyusermeta.h>
#endif

#include <iostream>
#include <mutex>
#include <ostream>
#include <stdexcept>
#include <vector>

#include <pthread.h>
#include "absl/status/status.h"

namespace hm {

absl::Status CustomAlgorithmBase::PostCapsInit(DSCustom_CreateParams* params) {
  HM_RETURN_IF_ERROR(DSCustomLibraryBase::PostCapsInit(params));

  GstStructure* s1 = NULL;
  GstCapsFeatures* feature;
  GstStructure* config = NULL;

  cuda_stream_ = params->m_cudaStream;
  assert(cuda_stream_); // temporary, maybe we dont even use cuda, just make sure its set if we do

  s1 = gst_caps_get_structure(m_inCaps, 0);

  feature = gst_caps_get_features(m_outCaps, 0);
  if (gst_caps_features_contains(feature, GST_CAPS_FEATURE_MEMORY_NVMM)) {
    hw_caps = true;
  } else {
    hw_caps = false;
  }

  // Create buffer pool
  // Set the params properly based on this custom library requirement
  params->m_bufferPoolConfig.cuda_mem_type = NVBUF_MEM_DEFAULT;
  if (hw_caps == false) {
    // pool_config.cuda_mem_type = NVBUF_MEM_SYSTEM;
    m_swbufpool = gst_buffer_pool_new();
    config = gst_buffer_pool_get_config(m_swbufpool);
    gfloat scale_factor = 1.5;
    if (m_outVideoFmt == GST_VIDEO_FORMAT_RGBA)
      scale_factor = 4;
    swbuffersize = m_outVideoInfo.width * m_outVideoInfo.height * scale_factor;
    gst_buffer_pool_config_set_params(config, m_outCaps, swbuffersize, 4, 4);
    if (!gst_buffer_pool_set_config(m_swbufpool, config)) {
      printf("FAILED TO SET CONFIG SW BUFFER POOL\n");
      return absl::InternalError("FAILED TO SET CONFIG SW BUFFER POOL");
    }
    gboolean is_active = gst_buffer_pool_set_active(m_swbufpool, TRUE);
    if (!is_active) {
      return absl::InternalError(" Failed to allocate the buffers inside the output pool");
    } else {
      GST_DEBUG_OBJECT(m_element, " Output SW buffer pool (%p) successfully created", m_swbufpool);
    }
  } else {
    if (params->m_bufferPoolConfig.max_buffers) {
      params->m_bufferPoolConfig.gpu_id = params->m_gpuId;
      assert(params->m_bufferPoolConfig.max_buffers);
      gst_structure_get_int(s1, "batch-size", &params->m_bufferPoolConfig.batch_size);

      if (params->m_bufferPoolConfig.batch_size == 0) {
        // If this component is placed before mux, batch-size value is not set
        // In this case make batch_size = 1
        params->m_bufferPoolConfig.batch_size = 1;
      }

      m_dsBufferPool = CreateBufferPool(&params->m_bufferPoolConfig, m_outCaps);
      if (!m_dsBufferPool) {
        return absl::InternalError("Custom Buffer Pool Creation failed");
      }
      m_config_params.compute_mode = NvBufSurfTransformCompute_GPU;
      m_config_params.gpu_id = params->m_gpuId;
      m_config_params.cuda_stream = params->m_cudaStream;
    }
  }

  m_outputThread = new std::thread(&CustomAlgorithmBase::OutputThread, this);
  m_buffer_pool_config = params->m_bufferPoolConfig;
  return absl::OkStatus();
}

// Return Compatible Output Caps based on input caps
GstCaps* CustomAlgorithmBase::GetCompatibleCaps(GstPadDirection direction, GstCaps* in_caps, GstCaps* othercaps) {
  GstStructure *s1, *s2;
  gint width = 0, height = 0;
  gint i = 0, num = 0, denom = 0;
  const gchar* inputFmt = NULL;
  const gchar* outputFmt = NULL;
  gint w = 0;
  gint h = 0;

  GstStructure *in_struct, *out_struct;
  const gchar *from_fmt = NULL, *to_fmt = NULL;

  GST_DEBUG_OBJECT(
      m_element,
      "\n----------\ndirection = %d (1=Src, 2=Sink) -> %s:\nCAPS = %s\n",
      direction,
      __func__,
      gst_caps_to_string(in_caps));
  GST_DEBUG_OBJECT(m_element, "%s : OTHERCAPS = %s\n", __func__, gst_caps_to_string(othercaps));

  GST_DEBUG_OBJECT(
      m_element, "trying to fixate othercaps %" GST_PTR_FORMAT " based on caps %" GST_PTR_FORMAT, othercaps, in_caps);

  othercaps = gst_caps_truncate(othercaps);
  othercaps = gst_caps_make_writable(othercaps);

  in_struct = gst_caps_get_structure(in_caps, 0);
  out_struct = gst_caps_get_structure(othercaps, 0);
  from_fmt = gst_structure_get_string(in_struct, "format");
  to_fmt = gst_structure_get_string(out_struct, "format");

  if (m_scaleFactor != 1)
    GST_DEBUG_OBJECT(m_element, "from_fmt = %s to_fmt = %s\n", from_fmt, to_fmt);

  // num_input_caps = gst_caps_get_size (in_caps);
  int num_output_caps = gst_caps_get_size(othercaps);

  // TODO: Currently it only takes first caps
  s1 = gst_caps_get_structure(in_caps, 0);
  for (i = 0; i < num_output_caps; i++) {
    s2 = gst_caps_get_structure(othercaps, i);
    inputFmt = gst_structure_get_string(s1, "format");
    outputFmt = gst_structure_get_string(s2, "format");

    GST_DEBUG_OBJECT(m_element, "InputFMT = %s \n\n", inputFmt);

    // Check for desired color format
    if ((strncmp(inputFmt, FORMAT_NV12, strlen(FORMAT_NV12)) == 0) ||
        (strncmp(inputFmt, FORMAT_I420, strlen(FORMAT_I420)) == 0) ||
        (strncmp(inputFmt, FORMAT_RGBA, strlen(FORMAT_RGBA)) == 0)) {
      // Set these output caps
      gst_structure_get_int(s1, "width", &width);
      gst_structure_get_int(s1, "height", &height);

      /* otherwise the dimension of the output heatmap needs to be fixated */

      // Here change the width and height on output caps based on the information provided
      // byt the custom library
      gst_structure_fixate_field_nearest_int(s2, "width", width);
      gst_structure_fixate_field_nearest_int(s2, "height", height);
      if (gst_structure_get_fraction(s1, "framerate", &num, &denom)) {
        gst_structure_fixate_field_nearest_fraction(s2, "framerate", num, denom);
      }

      // TODO: Get width, height, coloutformat, and framerate from customlibrary API
      // set the new properties accordingly
      gst_structure_get_int(s2, "width", &w);
      gst_structure_get_int(s2, "height", &h);

      /* If scalefactor is provided in the property then library expects no width/height
       * set on the SRC pad caps, output resolution will be according to the scalefactor.
       * If width/height is set on the SRC pad caps then library expects scalefactor to
       * be set as 1.0 .
       * */

      if ((m_scaleFactor != 1) && ((w != width) || (h != height))) {
        GST_ERROR_OBJECT(m_element, "Scalefactor should be 1.0 for explicit width height set on the SRC caps\n");
        return NULL;
      }

      if (m_scaleFactor != 1.0) {
        gst_structure_set(s2, "width", G_TYPE_INT, (gint)(m_scaleFactor * width), NULL);
        gst_structure_set(s2, "height", G_TYPE_INT, (gint)(m_scaleFactor * height), NULL);
      } else if (((w != 0) && (h != 0)) || (m_scaleFactor == 1.0)) {
        gst_structure_set(s2, "width", G_TYPE_INT, w, NULL);
        gst_structure_set(s2, "height", G_TYPE_INT, h, NULL);
      }

      if (!outputFmt) {
        gst_structure_set(s2, "format", G_TYPE_STRING, inputFmt, NULL);
      }
      gst_structure_set(s2, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1, NULL);

      GST_DEBUG_OBJECT(m_element, "%s : Updated OTHERCAPS = %s \n\n", __func__, gst_caps_to_string(othercaps));

      // Check if the mode is resulting into transform mode,
      // set the flag for transform mode accordingly
      if ((m_scaleFactor == 1) &&
          (!strcmp(gst_structure_get_string(s2, "format"), gst_structure_get_string(s1, "format"))) && (w == width) &&
          (h == height)) {
        m_transformMode = false;
      } else {
        m_transformMode = true;
      }

      break;
    } else {
      continue;
    }
  }
  return othercaps;
}

char* CustomAlgorithmBase::QueryProperties() {
  char* str = new char[1000];
  strcpy(
      str,
      "CUSTOM LIBRARY PROPERTIES\n \t\t\tcustomlib-props=\"scale-factor:x\" x = { 0 <= x <= 20}"
      "\n\t\t\tcustomlib-props=\"frame-insert-interval:x\" x is unsigned int");
  return str;
}

bool CustomAlgorithmBase::HandleEvent(GstEvent* event) {
  switch (GST_EVENT_TYPE(event)) {
    case GST_EVENT_EOS:
      m_processLock.lock();
      m_stop = TRUE;
      m_processCV.notify_all();
      m_processLock.unlock();
      while (outputthread_stopped == FALSE) {
        // g_print ("waiting for processq to be empty, buffers in processq = %ld\n", m_processQ.size());
        g_usleep(1000);
      }
      break;
    default:
      break;
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_STREAM_EOS) {
    gst_nvevent_parse_stream_eos(event, &source_id);
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_PAD_ADDED) {
    gst_nvevent_parse_pad_added(event, &source_id);
  }
  if ((GstNvEventType)GST_EVENT_TYPE(event) == GST_NVEVENT_PAD_DELETED) {
    gst_nvevent_parse_pad_deleted(event, &source_id);
  }
  return true;
}

// Set Custom Library Specific Properties
bool CustomAlgorithmBase::SetProperty(const Property& prop) {
  std::cout << "Inside Custom Lib : Setting Prop Key=" << prop.key << " Value=" << prop.value << std::endl;
  m_vectorProperty.emplace_back(prop.key, prop.value);

  try {
    if (prop.key.compare("scale-factor") == 0) {
      m_scaleFactor = stof(prop.value);
      if (m_scaleFactor == 0 || m_scaleFactor > 20 || m_scaleFactor < 0) {
        throw std::out_of_range("out of range scale factor");
      }
    }
    if (prop.key.compare("frame-insert-interval") == 0) {
      m_frameinsertinterval = stod(prop.value);
    }
  } catch (std::invalid_argument& e) {
    std::cout << "Invalid Scale Factor" << std::endl;
    return false;
  } catch (std::out_of_range& e) {
    std::cout << "Out of Range Scale Factor, provide between 0.0 and 20.0" << std::endl;
    return false;
  }

  return true;
}

/* Deinitialize the Custom Lib context */
CustomAlgorithmBase::~CustomAlgorithmBase() {
  Shutdown();
  if (m_swbufpool) {
    gst_buffer_pool_set_active(m_swbufpool, FALSE);
    gst_object_unref(m_swbufpool);
    m_swbufpool = NULL;
  }

  if (m_scratchNvBufSurface) {
    cudaFreeHost(m_scratchNvBufSurface);
    m_scratchNvBufSurface = NULL;
  }
}

// Returns NvDsBatchMeta if present in the gstreamer buffer else NULL
NvDsBatchMeta* CustomAlgorithmBase::GetNVDS_BatchMeta(GstBuffer* buffer) {
  gpointer state = NULL;
  GstMeta* gst_meta = NULL;
  NvDsBatchMeta* batch_meta = NULL;

  while ((gst_meta = gst_buffer_iterate_meta(buffer, &state))) {
    if (!gst_meta_api_type_has_tag(gst_meta->info->api, _dsmeta_quark)) {
      continue;
    }
    NvDsMeta* dsmeta = (NvDsMeta*)gst_meta;

    if (dsmeta->meta_type == NVDS_BATCH_GST_META) {
      if (batch_meta != NULL) {
        GST_WARNING("Multiple NvDsBatchMeta found on buffer %p", buffer);
      }
      batch_meta = (NvDsBatchMeta*)dsmeta->meta_data;
    }
  }

  return batch_meta;
}

/* Process Buffer */
BufferResult CustomAlgorithmBase::ProcessBuffer(GstBuffer* inbuf) {
  GstMapInfo in_map_info;
  NvBufSurface* in_surf;
  // BuffferResult result;
  NvDsBatchMeta* batch_meta = NULL;
  // int num_filled = 0;

  GST_DEBUG_OBJECT(m_element, "CustomLib: ---> Inside %s frame_num = %d\n", __func__, m_frameNum++);

  if (last_flow_ret_ == GST_FLOW_ERROR) {
    return BufferResult::Buffer_Error;
  }

  // TODO: End of Stream Handling
  memset(&in_map_info, 0, sizeof(in_map_info));

  /* Map the buffer contents and get the pointer to NvBufSurface. */
  if (!gst_buffer_map(inbuf, &in_map_info, GST_MAP_READ)) {
    GST_ELEMENT_ERROR(
        m_element, STREAM, FAILED, ("%s:gst buffer map to get pointer to NvBufSurface failed", __func__), (NULL));
    return BufferResult::Buffer_Error;
  }

  // Assuming that the plugin uses DS NvBufSurface data structure
  in_surf = (NvBufSurface*)in_map_info.data;

  gst_buffer_unmap(inbuf, &in_map_info);

#if 0
  // Batch meta is attached when nvstreammux plugin is present in the pipeline and
  // it should be upstream plugin in the pipeline
  if (batch_meta) {
    num_filled = batch_meta->num_frames_in_batch;
  } else {
    num_filled = 1;
  }

  GST_DEBUG ("\tCustomLib: Element : %s in_surf = %p\n", GST_ELEMENT_NAME(m_element), in_surf);
  GST_DEBUG ("\tCustomLib: Num of frames in batch: %d\n", num_filled);
#endif

  // Push buffer to process thread for further processing
  PacketInfo packetInfo;
  packetInfo.inbuf = inbuf;
  packetInfo.frame_num = m_frameNum;

  // Add custom preprocessing logic if required, here
  // Pass the buffer to output_loop for further processing and pusing to next component
  // Currently its just dumping few decoded video frames

  // Enable for dumping the input frame, for debugging purpose
  if (0) {
    batch_meta = GetNVDS_BatchMeta(inbuf);
    DumpNvBufSurface(in_surf, batch_meta);
  }

  m_processLock.lock();
  if (!outputthread_stopped) {
    m_processQ.push(packetInfo);
    m_processCV.notify_all();
  }
  m_processLock.unlock();

  return BufferResult::Buffer_Async; // BufferResult::Buffer_Ok;
}

void CustomAlgorithmBase::update_meta(NvDsBatchMeta* batch_meta, uint32_t icnt) {
  NvDsFrameMeta* frame_meta = NULL;

  gfloat scale_factor_width = 1, scale_factor_height = 1;
  if (m_scaleFactor == 1.0)
    return;
  scale_factor_width = scale_factor_height = m_scaleFactor;

  frame_meta = (NvDsFrameMeta*)g_list_nth_data(batch_meta->frame_meta_list, icnt);
  for (NvDsMetaList* l_obj = frame_meta->obj_meta_list; l_obj != NULL; l_obj = l_obj->next) {
    NvDsObjectMeta* object_meta = (NvDsObjectMeta*)(l_obj->data);
    object_meta->rect_params.left = scale_factor_width * object_meta->rect_params.left;
    object_meta->rect_params.top = scale_factor_height * object_meta->rect_params.top;
    object_meta->rect_params.width = scale_factor_width * object_meta->rect_params.width;
    object_meta->rect_params.height = scale_factor_height * object_meta->rect_params.height;
    object_meta->text_params.x_offset = scale_factor_width * object_meta->text_params.x_offset;
    object_meta->text_params.y_offset = scale_factor_height * object_meta->text_params.y_offset;
    object_meta->text_params.font_params.font_size =
        scale_factor_height * object_meta->text_params.font_params.font_size;
  }
}

#ifdef HAS_NVDS_CUSTOMUSERMETA
void* set_metadata_ptr() {
  guint i = 0;
  guint mem_count = 0;
  std::vector<faceboxes> fbs(2, {1, 2, 100, 200});

  NVDS_CUSTOM_PAYLOAD* custom_payload_fb = (NVDS_CUSTOM_PAYLOAD*)g_malloc0(sizeof(struct _NVDS_CUSTOM_PAYLOAD));
  if (fbs.size() > 0) {
    payload_type ptype = NVDS_PAYLOAD_TYPE_DUMMY_BBOX;

    custom_payload_fb->payloadType = ptype;
    custom_payload_fb->payloadSize = fbs.size() * sizeof(struct faceboxes);
    custom_payload_fb->payload = (uint8_t*)g_malloc0(fbs.size() * sizeof(struct faceboxes));

    faceboxes* f = (faceboxes*)custom_payload_fb->payload;
    for (i = 0; i < fbs.size(); i++) {
      auto& d = fbs[i];
      f->x = d.x;
      f->y = d.y;
      f->width = d.width;
      f->height = d.height;
      f++;
    }
    mem_count += (sizeof(uint32_t) + sizeof(uint32_t) + fbs.size() * (sizeof(struct faceboxes)));
    (void)mem_count;
  }

  return (gpointer)custom_payload_fb;
}

/* copy function set by user. "data" holds a pointer to NvDsUserMeta*/
gpointer copy_user_meta(gpointer data, gpointer user_data) {
  NvDsUserMeta* user_meta = (NvDsUserMeta*)data;
  NVDS_CUSTOM_PAYLOAD* udata = (NVDS_CUSTOM_PAYLOAD*)user_meta->user_meta_data;
  NVDS_CUSTOM_PAYLOAD* dst_user_metadata = (NVDS_CUSTOM_PAYLOAD*)g_malloc0(sizeof(struct _NVDS_CUSTOM_PAYLOAD));
  dst_user_metadata->payload = (uint8_t*)g_malloc0(udata->payloadSize);

  dst_user_metadata->payloadType = udata->payloadType;
  dst_user_metadata->payloadSize = udata->payloadSize;
  memcpy(dst_user_metadata->payload, udata->payload, udata->payloadSize);

  return (gpointer)dst_user_metadata;
}

/* release function set by user. "data" holds a pointer to NvDsUserMeta*/
void release_user_meta(gpointer data, gpointer user_data) {
  NvDsUserMeta* user_meta = (NvDsUserMeta*)data;
  if (user_meta->user_meta_data) {
    NVDS_CUSTOM_PAYLOAD* src_user_metadata = (NVDS_CUSTOM_PAYLOAD*)user_meta->user_meta_data;
    g_free(src_user_metadata->payload);
    src_user_metadata->payload = NULL;
    g_free(src_user_metadata);
    src_user_metadata = NULL;
  }
}

#define NUM_OBJECTS 5
void fill_dummy_batch_meta_on_buffer(NvDsBatchMeta* batch_meta) {
  NvDsMetaList* l_frame = NULL;
  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)(l_frame->data);
    /*printf ("pad_index = %d frame_width = %d frame_height = %d\n",
            frame_meta->pad_index, frame_meta->source_frame_width, frame_meta->source_frame_height);*/
    for (int i = 0; i < NUM_OBJECTS; i++) {
      NvDsObjectMeta* obj_meta = nvds_acquire_obj_meta_from_pool(batch_meta);
      obj_meta->unique_component_id = 0xAB;
      obj_meta->confidence = 0.0;

      /* This is an untracked object. Set tracking_id to -1. */
      obj_meta->object_id = UNTRACKED_OBJECT_ID;
      obj_meta->class_id = 2;

      NvOSD_RectParams& rect_params = obj_meta->rect_params;
      /* Assign bounding box coordinates. */
      rect_params.left = 10 + 10 * i;
      rect_params.top = 20 + 10 * i;
      rect_params.width = 100;
      rect_params.height = 100;
      rect_params.border_color.red = 1.0;
      rect_params.border_color.green = 0.5;
      rect_params.border_color.blue = 0.0;
      rect_params.border_width = 2;

      nvds_add_obj_meta_to_frame(frame_meta, obj_meta, NULL);
    }
  }
}

void update_dummy_meta_data_on_buffer(NvDsBatchMeta* batch_meta) {
  NvDsMetaList* l_frame = NULL;
  NvDsUserMeta* user_meta = NULL;
  NvDsMetaType user_meta_type = NVDS_DUMMY_BBOX_META;

  for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
    NvDsFrameMeta* frame_meta = (NvDsFrameMeta*)(l_frame->data);

    /* Acquire NvDsUserMeta user meta from pool */
    user_meta = nvds_acquire_user_meta_from_pool(batch_meta);

    /* Set NvDsUserMeta below */
    user_meta->user_meta_data = (void*)set_metadata_ptr();
    // g_print ("user meta data pointer = %p\n", user_meta->user_meta_data);
    user_meta->base_meta.meta_type = user_meta_type;
    user_meta->base_meta.copy_func = (NvDsMetaCopyFunc)copy_user_meta;
    user_meta->base_meta.release_func = (NvDsMetaReleaseFunc)release_user_meta;

    /* We want to add NvDsUserMeta to frame level */
    nvds_add_user_meta_to_frame(frame_meta, user_meta);
  }
}
#endif // HAS_NVDS_CUSTOMUSERMETA

void CustomAlgorithmBase::Shutdown() {
  std::unique_lock<std::mutex> lk(m_processLock);
  // Send a tombstone
  if (!outputthread_stopped) {
    // m_processQ.emplace(PacketInfo{.inbuf=nullptr});
    // std::cout << "Process Q Empty : " << m_processQ.empty() << std::endl;
    m_processCV.wait(lk, [&] { return m_processQ.empty(); });
    m_stop = TRUE;
    m_processCV.notify_all();
    lk.unlock();
  }
  /* Wait for OutputThread to complete */
  if (m_outputThread && m_outputThread->joinable()) {
    m_outputThread->join();
  }
}

/* Output Processing Thread */
void CustomAlgorithmBase::OutputThread(void) {
  GstFlowReturn flow_ret;
  GstBuffer* outBuffer = NULL;
  pthread_setname_np(pthread_self(), "VIDEOPREP-OUT");
  std::unique_lock<std::mutex> lk(m_processLock);
  NvDsBatchMeta* batch_meta = NULL;
  if (hw_caps == true) {
    cudaError_t cuErr = cudaSetDevice(m_gpuId);
    if (cuErr != cudaSuccess) {
      GST_ERROR_OBJECT(m_element, "Unable to set cuda device");
      return;
    }
  }
  /* Run till signalled to stop. */
  while (1) {
    /* Wait if processing queue is empty. */
    if (m_processQ.empty()) {
      if (m_stop == TRUE) {
        break;
      }
      m_processCV.wait(lk);
      continue;
    }

    PacketInfo packetInfo = m_processQ.front();
    m_processQ.pop();

    m_processCV.notify_all();
    lk.unlock();

    // Add custom algorithm logic here
    // Once buffer processing is done, push the buffer to the downstream by using gst_pad_push function

    NvBufSurface* in_surf = getNvBufSurface(packetInfo.inbuf);

    /* Insert custom buffer every after 10 frames */
    if (m_frameinsertinterval) {
      if ((packetInfo.frame_num % m_frameinsertinterval) == 0) {
        auto status = InsertCustomFrame(&packetInfo);
        if (!status.ok()) {
          std::cerr << status << std::endl;
          update_last_flow_ret(GST_FLOW_ERROR);
          // Continue and let the last_status stop us eventually
        }
      }
    }

    bool send_eos = false;

    if (m_transformMode) {
      if (hw_caps == true) {
        // Transform mode, hence transform input buffer to output buffer
        GstBuffer* newGstOutBuf = NULL;
        GstFlowReturn result = GST_FLOW_OK;
        assert(m_dsBufferPool);
        result = gst_buffer_pool_acquire_buffer(m_dsBufferPool, &newGstOutBuf, NULL);
        if (result != GST_FLOW_OK) {
          GST_ERROR_OBJECT(m_element, "InsertCustomFrame failed error = %d, exiting...", result);
          update_last_flow_ret(GST_FLOW_ERROR);
        }
        // Copy meta and transform if required
        if (!gst_buffer_copy_into(newGstOutBuf, packetInfo.inbuf, GST_BUFFER_COPY_META, 0, -1)) {
          GST_DEBUG_OBJECT(m_element, "Buffer metadata copy failed \n");
        }

        nvds_set_input_system_timestamp(newGstOutBuf, GST_ELEMENT_NAME(m_element));
        // Copy previous buffer to new buffer, repreat the frame
        NvBufSurface* out_surf = getNvBufSurface(newGstOutBuf);
        if (!in_surf || !out_surf) {
          GST_ERROR_OBJECT(m_element, "CustomLib: NvBufSurface not found in the buffer...exiting...\n");
          update_last_flow_ret(GST_FLOW_ERROR);
        }

        batch_meta = GetNVDS_BatchMeta(newGstOutBuf);
        for (uint32_t icnt = 0; icnt < in_surf->numFilled; icnt++) {
          if (batch_meta)
            update_meta(batch_meta, icnt);
        }
#ifdef HAS_NVDS_CUSTOMUSERMETA
        if (m_dummyMetaInsert && batch_meta) {
          update_dummy_meta_data_on_buffer(batch_meta);
        }
        if (m_fillDummyBatchMeta && batch_meta) {
          fill_dummy_batch_meta_on_buffer(batch_meta);
        }
#endif // HAS_NVDS_CUSTOMUSERMETA
        assert(m_element);
        assert(cuda_stream_);
        if (in_surf && out_surf) {
          cuda_status.Update(GenerateOutput(batch_meta, in_surf, out_surf));
          if (!cuda_status.ok()) {
            std::cerr << cuda_status << std::endl;
            if (cuda_status.code() == absl::StatusCode::kCancelled) {
              // update_last_flow_ret(GST_FLOW_EOS);
              send_eos = true;
            } else {
              update_last_flow_ret(GST_FLOW_ERROR);
            }
          }
        }

        outBuffer = newGstOutBuf;

        if (newGstOutBuf) {
          GST_BUFFER_PTS(outBuffer) = GST_BUFFER_PTS(packetInfo.inbuf);
        }
        // Unref the input buffer
        gst_buffer_unref(packetInfo.inbuf);
      } else if (hw_caps == false) {
        /*
         * Note: This is SRC pad of nvdsvideotemplate where SW caps are negotiated.
         * Here SW buffer pool is allocated and input buffer is copied into outgoing buffer
         * acquired from the SW buffer pool without any conversion.
         * This would lead to corrupted output if scalefactor is not set to 1.0
         *
         * It is recommended for the user to implement the actual conversion rotines before
         * copying the buffer.
         * */
        GstBuffer* swGstOutBuf = NULL;
        GstMapInfo swbufmap = GST_MAP_INFO_INIT;
        GstMapInfo inbufmap = GST_MAP_INFO_INIT;
        GstFlowReturn result = GST_FLOW_OK;
        result = gst_buffer_pool_acquire_buffer(m_swbufpool, &swGstOutBuf, NULL);
        if (result != GST_FLOW_OK) {
          GST_ERROR_OBJECT(m_element, "Acquire buffer failed with error = %d", result);
          return;
        }

        if (!gst_buffer_map(swGstOutBuf, &swbufmap, GST_MAP_READ)) {
          printf("could not map sw buffer acquired from sw buffer pool \n");
          return;
        }
        if (!gst_buffer_map(packetInfo.inbuf, &inbufmap, GST_MAP_READ)) {
          printf("could not map buffer buffer \n");
          return;
        }
        guint buffersize = (gst_buffer_get_size(packetInfo.inbuf) > gst_buffer_get_size(swGstOutBuf))
            ? gst_buffer_get_size(swGstOutBuf)
            : gst_buffer_get_size(packetInfo.inbuf);
        memcpy(swbufmap.data, inbufmap.data, buffersize);

        gst_buffer_unmap(swGstOutBuf, &swbufmap);
        gst_buffer_unmap(packetInfo.inbuf, &inbufmap);

        outBuffer = swGstOutBuf;
        GST_BUFFER_PTS(outBuffer) = GST_BUFFER_PTS(packetInfo.inbuf);
        if (!gst_buffer_copy_into(swGstOutBuf, packetInfo.inbuf, GST_BUFFER_COPY_META, 0, -1)) {
          GST_DEBUG_OBJECT(m_element, "Buffer metadata copy failed \n");
        }
        // Unref the input buffer
        gst_buffer_unref(packetInfo.inbuf);
      }
    } else {
      // Transform IP case
      outBuffer = packetInfo.inbuf;
      assert(cuda_stream_);
      batch_meta = GetNVDS_BatchMeta(outBuffer);
      cuda_status.Update(GenerateOutput(batch_meta, in_surf, nullptr));
      if (!cuda_status.ok()) {
        std::cerr << cuda_status << std::endl;
        update_last_flow_ret(GST_FLOW_ERROR);
      }
      nvds_set_input_system_timestamp(outBuffer, GST_ELEMENT_NAME(m_element));
    }
    if (last_flow_ret_ == GST_FLOW_OK) {
      // Do we need this anymore?
      if (!send_eos) {
        nvds_set_output_system_timestamp(outBuffer, GST_ELEMENT_NAME(m_element));
        flow_ret = gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(m_element), outBuffer);
        GST_DEBUG_OBJECT(
            m_element,
            "CustomLib: %s in_surf=%p, Pushing Frame %d to downstream... flow_ret = %d TS=%" GST_TIME_FORMAT " \n",
            __func__,
            in_surf,
            packetInfo.frame_num,
            flow_ret,
            GST_TIME_ARGS(GST_BUFFER_PTS(outBuffer)));
      } else {
        if (!eos_sent_) {
          GstEvent* eos_event = gst_event_new_eos();
          gboolean ret = gst_pad_push_event(GST_BASE_TRANSFORM_SRC_PAD(m_element), eos_event);
          if (!ret) {
            std::cerr << "Error sending EOS downstream from videoprep algorithm" << std::endl;
          } else {
            eos_sent_ = true;
          }
        }
        gst_buffer_unref(outBuffer);
      }
    }
    lk.lock();
    continue;
  }
  outputthread_stopped = true;
  lk.unlock();
  return;
}

// Insert Custom Frame
absl::Status CustomAlgorithmBase::InsertCustomFrame(PacketInfo* packetInfo) {
  GstBuffer* newGstOutBuf = NULL;
  GstFlowReturn result = GST_FLOW_OK;

  result = gst_buffer_pool_acquire_buffer(m_dsBufferPool, &newGstOutBuf, NULL);
  if (result != GST_FLOW_OK) {
    GST_ERROR_OBJECT(m_element, "InsertCustomFrame failed error = %d, exiting...", result);
    return absl::InternalError("InsertCustomFrame failed");
  }
  // TODO: Copy meta and transform if required
  if (!gst_buffer_copy_into(newGstOutBuf, packetInfo->inbuf, GST_BUFFER_COPY_META, 0, -1)) {
    GST_DEBUG_OBJECT(m_element, "Buffer metadata copy failed\n");
    return absl::InternalError("Buffer metadata copy failed");
  }

  // Copy previous buffer to new buffer, repreat the frame
  NvBufSurface* in_surf = getNvBufSurface(packetInfo->inbuf);
  NvBufSurface* out_surf = getNvBufSurface(newGstOutBuf);
  if (!in_surf || !out_surf) {
    GST_DEBUG_OBJECT(m_element, "CustomLib: NvBufSurface not found in the buffer...exiting...\n");
    return absl::InternalError("CustomLib: NvBufSurface not found in the buffer");
  }
  // Enable below code to copy the frame, else it will insert GREEN frame
  if (0) {
    NvBufSurfaceCopy(in_surf, out_surf);
  }

  out_surf->numFilled = in_surf->numFilled;

  // TODO: Do Timestamp management,
  // currently setting is 10ms lesser than next buffer for this newly inserting frame
  GST_BUFFER_PTS(newGstOutBuf) = GST_BUFFER_PTS(packetInfo->inbuf) - 10000000;

  // Increment frame count
  m_frameNum++;

  // Check the surface we are psuhing downstream is of expected width and height values
  // else assert
  g_assert((guint)m_outVideoInfo.width == out_surf->surfaceList->width);
  g_assert((guint)m_outVideoInfo.height == out_surf->surfaceList->height);

  GST_DEBUG_OBJECT(
      m_element,
      "CustomLib: --- Pushed New Custom Buffer %s : InBuf_TS=%" GST_TIME_FORMAT
      " GstOutBuf=%p CustomFrame TS=%" GST_TIME_FORMAT " \n",
      __func__,
      GST_TIME_ARGS(GST_BUFFER_PTS(packetInfo->inbuf)),
      newGstOutBuf,
      GST_TIME_ARGS(GST_BUFFER_PTS(newGstOutBuf)));

  result = gst_pad_push(GST_BASE_TRANSFORM_SRC_PAD(m_element), newGstOutBuf);
  if (result != GST_FLOW_OK) {
    GST_ERROR_OBJECT(m_element, "custom buffer pad push failed error = %d\n", result);
    return absl::InternalError("custom buffer pad push failed");
  }
  return absl::OkStatus();
}

// Helper function to dump the nvbufsurface, used for debugging purpose
void CustomAlgorithmBase::DumpNvBufSurface(NvBufSurface* in_surface, NvDsBatchMeta* batch_meta) {
  guint numFilled = in_surface->numFilled;
  guint source_id = 0;
  guint i = 0;
  std::ofstream outfile;
  void* input_data = NULL;
  guint size = 0;

  if (dump_max_frames) {
    dump_max_frames--;
  } else {
    return;
  }

  for (i = 0; i < numFilled; i++) {
    if (batch_meta) {
      NvDsFrameMeta* frame_meta = nvds_get_nth_frame_meta(batch_meta->frame_meta_list, i);
      source_id = frame_meta->pad_index;
    }

    std::string tmp = "_" + std::to_string(in_surface->surfaceList[i].width) + "x" +
        std::to_string(in_surface->surfaceList[i].height) + "_" + "BS-" + std::to_string(source_id);

    input_data = in_surface->surfaceList[i].dataPtr;

    switch (m_inVideoFmt) {
      case GST_VIDEO_FORMAT_NV12: {
        std::string fname = GST_ELEMENT_NAME(m_element) + tmp + ".nv12";

        size = (in_surface->surfaceList[i].width * in_surface->surfaceList[i].height * 3) / 2;

        if (m_scratchNvBufSurface == NULL) {
          ck(cudaMallocHost(&m_scratchNvBufSurface, size));
          outfile.open(fname, std::ofstream::out);
        } else {
          outfile.open(fname, std::ofstream::out | std::ofstream::app);
        }

        cudaMemcpy2D(
            m_scratchNvBufSurface,
            in_surface->surfaceList[i].width,
            input_data,
            in_surface->surfaceList[i].width,
            in_surface->surfaceList[i].width,
            (in_surface->surfaceList[i].height * 3) / 2,
            cudaMemcpyDeviceToHost);

        outfile.write(reinterpret_cast<char*>(m_scratchNvBufSurface), size);
        outfile.close();
      } break;

      case GST_VIDEO_FORMAT_RGBA:
      case GST_VIDEO_FORMAT_BGRx: {
        std::string fname = GST_ELEMENT_NAME(m_element) + tmp;

        if (m_inVideoFmt == GST_VIDEO_FORMAT_RGBA)
          fname.append(".rgba");
        else if (m_inVideoFmt == GST_VIDEO_FORMAT_BGRx)
          fname.append(".bgrx");

        size = in_surface->surfaceList[i].width * in_surface->surfaceList[i].height * 4;

        if (m_scratchNvBufSurface == NULL) {
          ck(cudaMallocHost(&m_scratchNvBufSurface, size));
          outfile.open(fname, std::ofstream::out);
        } else {
          outfile.open(fname, std::ofstream::out | std::ofstream::app);
        }

        cudaMemcpy2D(
            m_scratchNvBufSurface,
            in_surface->surfaceList[i].width * 4,
            input_data,
            in_surface->surfaceList[i].width * 4,
            in_surface->surfaceList[i].width * 4,
            in_surface->surfaceList[i].height,
            cudaMemcpyDeviceToHost);

        outfile.write(reinterpret_cast<char*>(m_scratchNvBufSurface), size);
        outfile.close();
      } break;

      default:
        GST_ERROR_OBJECT(m_element, "CustomLib: %s : NOT SUPPORTED FORMAT\n", GST_ELEMENT_NAME(m_element));
        break;
    }
  }
}

} // namespace hm
