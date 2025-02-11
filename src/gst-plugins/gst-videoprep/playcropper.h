#pragma once

namespace hm {

/**
 * @addtogroup three Standard GStreamer boilerplate
 * @{
 */
#define GST_TYPE_PLAY_CROPPER \
  (hm::videoprep::gst_videoprep_get_type())
#define GST_VIDEOPREP_PLAY_CROPPER(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PLAY_CROPPER,GstVideoPrepPlayCropper))
#define GST_VIDEOPREP_PLAY_CROPPER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PLAY_CROPPER,GstVideoPrepPlayCropperClass))
#define GST_IS_VIDEOPREP_PLAY_CROPPER(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PLAY_CROPPER))
#define GST_IS_VIDEOPREP_PLAY_CROPPER_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PLAY_CROPPER))

}
