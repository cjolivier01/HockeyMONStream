#pragma once

#include "hstream/src/libs/common/Surface.h"
#include "includes/hmcustomlib_interface.hpp"

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>
#include <gst/video/gstvideometa.h>
#include <gst/video/video.h>

#include <cuda.h>
#include <npp.h>

#include "nvbufsurface.h"
#include "nvbufsurftransform.h"

#include "preputils.h"

#include "includes/hmcustomlib_base.hpp"

#include "absl/status/status.h"

#include <cassert>

namespace hm {
namespace videoprep {

class VideoPrepPriv : public DSCustomLibraryBase {
 public:
  VideoPrepPriv(int gpu_id, size_t batch_size) : scratch_buffers(gpu_id, batch_size) {}
  hm::surface::SurfaceList scratch_buffers;

  bool render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
    return render_.render(name, surface, stream);
  }

  // -DSCustomLibraryBase
  bool SetProperty(const Property& prop) override {
    assert(false);
    return true;
  }

  bool HandleEvent(GstEvent* event) override {
    return true;
  }

  char* QueryProperties() override {
    assert(false);
    // ugh @ c programmers
    return strdup("");
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override {
    assert(false);
    return BufferResult::Buffer_Ok;
  }

  // DSCustomLibraryBase-

  virtual absl::Status GenerateOutput(
      NvDsBatchMeta* batch_meta,
      NvBufSurface* in_surface,
      NvBufSurface* out_surface) {
    return absl::UnimplementedError("GenerateOutput is not implemented for VideoPrepPriv");
  }

  virtual gint AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) {
    return 0;
  }

  void SetPrivateConfig(const char* config_string);

  GstFlowReturn get_last_flow_ret() const {
    return last_flow_ret_;
  }

  virtual void Shutdown() {}

 protected:
  void update_last_flow_ret(GstFlowReturn r) {
    if (last_flow_ret_ != GST_FLOW_ERROR) {
      // Don't allow to set from error to non-error
      last_flow_ret_ = r;
    }
  }

  GstFlowReturn last_flow_ret_{GST_FLOW_OK};
  RenderSet render_;
};

} // namespace videoprep
} // namespace hm
