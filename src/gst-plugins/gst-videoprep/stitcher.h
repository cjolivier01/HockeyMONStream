#pragma once

#include "cudaPano.h"
#include "gstvideoprep.h"

#include <mutex>

namespace hm {
namespace stitcher {

/**
 * @addtogroup three Standard GStreamer boilerplate
 * @{
 */
// #define GST_TYPE_PLAY_CROPPER (hm::videoprep::gst_videoprep_get_type())
// #define GST_VIDEOPREP_PLAY_CROPPER(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PLAY_CROPPER, GstVideoPrepStitcher))
// #define GST_VIDEOPREP_PLAY_CROPPER_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PLAY_CROPPER, GstVideoPrepStitcherClass))
// #define GST_IS_VIDEOPREP_PLAY_CROPPER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PLAY_CROPPER))
// #define GST_IS_VIDEOPREP_PLAY_CROPPER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PLAY_CROPPER))

class StitcherPriv : public videoprep::VideoPrepPriv {
  using Super = videoprep::VideoPrepPriv;

 public:
  StitcherPriv(int gpu_id, size_t batch_size) : videoprep::VideoPrepPriv(gpu_id, batch_size) {}
  ~StitcherPriv();

  // template <typename... Args>
  // void render(Args&&... args) {
  //     // Forward all arguments to the target function
  //     render_(std::forward<Args>(args)...);
  // }

  bool render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
    return render_.render(name, surface, stream);
  }

  // -DSCustomLibraryBase

  bool SetInitParams(DSCustom_CreateParams* params) override;

  bool SetProperty(const Property& prop) override {
    assert(false);
    return true;
  }

  bool HandleEvent(GstEvent* event) override {
    assert(false);
    return true;
  }

  char* QueryProperties() override {
    assert(false);
    return strdup("");
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override {
    assert(false);
    return BufferResult::Buffer_Ok;
  }

  // DSCustomLibraryBase-

  cudaError GenerateOutput(
      NvDsBatchMeta* batch_meta,
      videoprep::GstVideoPrep* videoprep,
      NvBufSurface* in_surface,
      NvBufSurface* out_surface) override;

 private:
  std::unique_ptr<hm::pano::cuda::CudaStitchPano<uchar4, float3>> stitcher_;
  std::mutex process_mu_;
};

/** GStreamer boilerplate. */
struct GstVideoPrepStitcher : public videoprep::GstVideoPrep {
  // Don't add stuff here
  GstVideoPrepStitcher() {
    static_assert(sizeof(GstVideoPrepStitcher) == sizeof(videoprep::GstVideoPrep));
  }
};

struct GstVideoPrepStitcherClass : public videoprep::GstVideoPrepClass {
  // Don't add stuff here
  GstVideoPrepStitcherClass() {
    static_assert(sizeof(GstVideoPrepStitcherClass) == sizeof(videoprep::GstVideoPrepClass));
  }
};

} // namespace stitcher
} // namespace hm
