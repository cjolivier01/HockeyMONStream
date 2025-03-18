#pragma once

#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/hmcustomlib_interface.hpp"

#include <cuda.h>
#include <cuda_runtime.h>
#include <gstreamer-1.0/gst/gstpad.h>
#include <string.h>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include "VideoPrepPriv.h"
#include "gst-nvevent.h"
#include "gstnvdsmeta.h"
#include "nvbufsurface.h"
#include "nvbufsurftransform.h"
#include "nvdscustomusermeta.h"
#include "nvdsdummyusermeta.h"

// #include "includes/hmcustomlib_base.hpp"

namespace hm {

#define FORMAT_NV12 "NV12"
#define FORMAT_RGBA "RGBA"
#define FORMAT_I420 "I420"
#define GST_CAPS_FEATURE_MEMORY_NVMM "memory:NVMM"

void update_dummy_meta_data_on_buffer(NvDsBatchMeta* batch_meta);
void* set_metadata_ptr(void);
gpointer copy_user_meta(gpointer data, gpointer user_data);
void release_user_meta(gpointer data, gpointer user_data);
void fill_dummy_batch_meta_on_buffer(NvDsBatchMeta* batch_meta);

inline bool CHECK__(int e, int iLine, const char* szFile) {
  if (e != cudaSuccess) {
    std::cout << "CUDA runtime error " << e << " at line " << iLine << " in file " << szFile;
    exit(-1);
    return false;
  }
  return true;
}
#define ck(call) CHECK__(call, __LINE__, __FILE__)

/* This quark is required to identify NvDsMeta when iterating through
 * the buffer metadatas */
static GQuark _dsmeta_quark = g_quark_from_static_string(NVDS_META_STRING);

/* Strcture used to share between the threads */
struct PacketInfo {
  GstBuffer* inbuf;
  guint frame_num;
};

class CustomAlgorithmBase : public videoprep::VideoPrepPriv {
 public:
  CustomAlgorithmBase(int gpu_id, size_t batch_size) : videoprep::VideoPrepPriv(gpu_id, batch_size) {
    m_vectorProperty.clear();
    outputthread_stopped = false;
  }

  /* Set Init Parameters */
  absl::Status PostCapsInit(DSCustom_CreateParams* params) override;

  /* Set Custom Properties  of the library */
  bool SetProperty(const Property& prop) override;

  /* Pass GST events to the library */
  bool HandleEvent(GstEvent* event) override;

  char* QueryProperties() override;

  /* Process Incoming Buffer */
  BufferResult ProcessBuffer(GstBuffer* inbuf) override;

  void Shutdown() override;

  /* Retrun Compatible Caps */
  GstCaps* GetCompatibleCaps(GstPadDirection direction, GstCaps* in_caps, GstCaps* othercaps) override;

  gboolean hw_caps;

  /* Deinit members */
  ~CustomAlgorithmBase();

 private:
  /* Helper Function to Extract Batch Meta from buffer */
  NvDsBatchMeta* GetNVDS_BatchMeta(GstBuffer* buffer);

  /* Output Processing Thread, push buffer to downstream  */
  void OutputThread(void);

  /* Helper function to Dump NvBufSurface RAW content */
  void DumpNvBufSurface(NvBufSurface* in_surface, NvDsBatchMeta* batch_meta);

  /* Insert Custom Frame */
  void InsertCustomFrame(PacketInfo* packetInfo);

  void update_meta(NvDsBatchMeta* batch_meta, uint32_t icnt);

 public:
  videoprep::GstVideoPrep* videoprep_;
  guint source_id = 0;
  guint m_frameNum = 0;
  gdouble m_scaleFactor = 1.0;
  guint m_frameinsertinterval = 0;
  bool m_transformMode = false;
  bool outputthread_stopped = false;

  /* Custom Library Bufferpool */
  BufferPoolConfig m_buffer_pool_config{
      0,
  };
  GstBufferPool* m_dsBufferPool = NULL;
  GstBufferPool* m_swbufpool = NULL;
  guint swbuffersize;

  /* Output Thread Pointer */
  std::thread* m_outputThread = NULL;

  /* Queue and Lock Management */
  std::queue<PacketInfo> m_processQ;
  std::mutex m_processLock;
  std::condition_variable m_processCV;
  cudaStream_t cuda_stream_{0};
  absl::Status cuda_status;
  NvBufSurfTransformConfigParams m_config_params;
  /* Aysnc Stop Handling */
  gboolean m_stop = FALSE;
  bool eos_sent_;

  /* Vector Containing Key:Value Pair of Custom Lib Properties */
  std::vector<Property> m_vectorProperty;

  void* m_scratchNvBufSurface = NULL;

  // Currently dumps first 5 input video frame into file for demonstration purpose
  // Use vooya or simillar player to view NV12 / RGBA video raw frame
  int dump_max_frames = 5;
};

} // namespace hm
