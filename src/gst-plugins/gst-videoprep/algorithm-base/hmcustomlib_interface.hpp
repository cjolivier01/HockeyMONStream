#pragma once

#include <cuda_runtime.h>
#include <gst/base/gstbasetransform.h>
#include <gst/gstbuffer.h>

#include <optional>
#include <string>

#include "absl/status/status.h"

namespace hm {

namespace videoprep {
struct GstVideoPrep;
}

enum class BufferResult {
  Buffer_Ok, // Push the buffer from submit_input function
  Buffer_Drop, // Drop the buffer inside submit_input function
  Buffer_Async, // Return from submit_input function, custom lib to push the buffer
  Buffer_Error // Error occured
};

struct BufferPoolConfig {
  gint cuda_mem_type{0};
  guint gpu_id{0};
  guint max_buffers{0};
  gint batch_size{0};
};

struct DSCustom_CreateParams {
  BufferPoolConfig m_bufferPoolConfig;
  size_t output_width_height[2] = {0};
  gchar* config_file{nullptr};
  GstBaseTransform* m_element{nullptr};
  GstCaps* m_inCaps{nullptr};
  GstCaps* m_outCaps{nullptr};
  guint m_gpuId{0};
  cudaStream_t m_cudaStream{nullptr};
  gboolean m_dummyMetaInsert{false};
  gboolean m_fillDummyBatchMeta{false};
  GstBufferPool* m_bufferPool{nullptr};
};

struct Property {
  Property(std::string arg_key, std::string arg_value) : key(arg_key), value(arg_value) {}

  std::string key;
  std::string value;
};

class IDSCustomLibrary {
 public:
  virtual absl::Status PreCapsInit(DSCustom_CreateParams* params) = 0;
  virtual absl::Status PostCapsInit(DSCustom_CreateParams* params) = 0;
  virtual bool SetProperty(const Property& prop) = 0;
  virtual bool HandleEvent(GstEvent* event) = 0;
  virtual char* QueryProperties() = 0;
  virtual GstCaps* GetCompatibleCaps(GstPadDirection direction, GstCaps* in_caps, GstCaps* othercaps) = 0;
  virtual BufferResult ProcessBuffer(GstBuffer* inbuf) = 0;
  virtual ~IDSCustomLibrary() = default;
};

} // namespace hm
