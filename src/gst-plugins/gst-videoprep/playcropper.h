#pragma once

#include "gstvideoprep.h"

namespace hm {

/**
 * @addtogroup three Standard GStreamer boilerplate
 * @{
 */
#define GST_TYPE_PLAY_CROPPER (hm::videoprep::gst_videoprep_get_type())
#define GST_VIDEOPREP_PLAY_CROPPER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), GST_TYPE_PLAY_CROPPER, GstVideoPrepPlayCropper))
#define GST_VIDEOPREP_PLAY_CROPPER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass), GST_TYPE_PLAY_CROPPER, GstVideoPrepPlayCropperClass))
#define GST_IS_VIDEOPREP_PLAY_CROPPER(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), GST_TYPE_PLAY_CROPPER))
#define GST_IS_VIDEOPREP_PLAY_CROPPER_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), GST_TYPE_PLAY_CROPPER))

class PlayCropperPriv : public videoprep::VideoPrepPriv {
 public:
  PlayCropperPriv(int gpu_id, size_t batch_size) : videoprep::VideoPrepPriv(gpu_id, batch_size) {}

  // template <typename... Args>
  // void render(Args&&... args) {
  //     // Forward all arguments to the target function
  //     render_(std::forward<Args>(args)...);
  // }

  void render(const std::string& name, hm::surface::Surface surface, cudaStream_t stream) {
    return render_.render(name, surface, stream);
  }

  // -DSCustomLibraryBase
  bool SetProperty(Property& prop) override {
    assert(false);
    return true;
  }

  bool HandleEvent(GstEvent* event) override  {
    assert(false);
    return true;
  }

  const char* QueryProperties() override {
    assert(false);
    return "";
  }

  BufferResult ProcessBuffer(GstBuffer* inbuf) override {
    assert(false);
    return BufferResult::Buffer_Ok;
  }

  // DSCustomLibraryBase-

 protected:
};

/** GStreamer boilerplate. */
struct GstVideoPrepPlayCropper : public videoprep::GstVideoPrep {

};

struct GstVideoPrepPlayCropperClass : public videoprep::GstVideoPrepClass {

};

} // namespace hm
