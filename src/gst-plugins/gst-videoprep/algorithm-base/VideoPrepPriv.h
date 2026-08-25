#pragma once

#include "hstream/src/libs/common/RenderSet.h"
#include "hstream/src/libs/common/Surface.h"

// #include "deepstream/sources/includes/nvbufsurface.h"
#include <cassert>
#include <cstddef>
#include <functional>
#include "nvbufsurface.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "hstream/src/gst-plugins/gst-videoprep/algorithm-base/hmcustomlib_base.hpp"
#include "nvdsmeta.h"

namespace hm {
namespace videoprep {

struct RuntimeOutputSize {
  size_t width{0};
  size_t height{0};
  guint batch_size{0};

  bool valid() const {
    return width > 0 && height > 0;
  }
};

enum class RuntimeOutputPoolStatusDisposition {
  kProceed,
  kDefer,
  kSendEos,
  kError,
};

absl::Status runtime_output_pool_deferred_status(const std::string& reason);
bool is_runtime_output_pool_deferred_status(const absl::Status& status);
RuntimeOutputPoolStatusDisposition classify_runtime_output_pool_status(const absl::Status& status);

class RuntimeOutputPoolFlow {
 public:
  // Returns true after consuming input_buffer for a terminal EOS status.
  // The caller must then skip output-pool acquisition and output-buffer access.
  bool handle_status(const absl::Status& status, GstBuffer* input_buffer, const std::function<void()>& send_eos);
  // Once sizing ends at EOS, every later input is consumed before its surface,
  // metadata, output pool, or output buffer can be accessed.
  bool consume_if_terminal(GstBuffer* input_buffer) const;
  // Completes a generated-output cancellation by sending EOS once, releasing
  // the unused output, and making all later output production terminal.
  void finish_with_eos(GstBuffer* output_buffer, const std::function<void()>& send_eos);
  bool eos_terminal() const {
    return eos_terminal_;
  }

 private:
  bool eos_terminal_{false};
};

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

  virtual absl::Status GenerateOutput(NvDsBatchMeta* batch_meta, NvBufSurface* in_surface, NvBufSurface* out_surface) {
    return absl::UnimplementedError("GenerateOutput is not implemented for VideoPrepPriv");
  }

  virtual gint AllocateScratchBuffers(videoprep::GstVideoPrep* videoprep) {
    return 0;
  }

  virtual bool UsesRuntimeOutputSize() const {
    return false;
  }

  // Once a runtime-sized algorithm discovers its output dimensions, expose
  // them to GstBaseTransform negotiation so a later RECONFIGURE cannot fall
  // back to the input dimensions.
  virtual RuntimeOutputSize RuntimeOutputSizeForNegotiation() const {
    return {};
  }

  virtual guint GetOutputBatchSize(guint input_batch_size, guint configured_batch_size) const {
    return configured_batch_size;
  }

  virtual absl::StatusOr<RuntimeOutputSize> PrepareRuntimeOutputSize(
      NvDsBatchMeta* batch_meta,
      NvBufSurface* in_surface) {
    return RuntimeOutputSize{};
  }

  bool SetPrivateConfig(const char* config_string);

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
