#include "gstvideoprep.h"
#include "playcropper.h"

namespace hm {
namespace videoprep {

GST_DEBUG_CATEGORY_STATIC(gst_videoprep_debug);
GST_DEBUG_CATEGORY_STATIC(gst_playcropper_debug);

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean videoprep_init(GstPlugin* plugin) {
  /* debug category for filtering log messages
   *
   * exchange the string 'Template videoprep' with your description
   */
  GST_DEBUG_CATEGORY_INIT(gst_videoprep_debug, "videoprep", 0, "videoprep");

  gboolean result = gst_element_register(plugin, "videoprep", GST_RANK_NONE, GST_TYPE_VIDEOPREP);
  result |= gst_element_register(plugin, "playcropper", GST_RANK_NONE, GST_TYPE_PLAY_CROPPER);
}
} // namespace videoprep
} // namespace hm

#ifndef PACKAGE
#define PACKAGE "videoprep"
#endif

#define PACKAGE_DESCRIPTION "Gstreamer plugin to dewarp 360d surfaces"
#define PACKAGE_LICENSE "Proprietary"
#define PACKAGE_NAME "GStreamer nVidia Dewarper Plugin"
#define PACKAGE_URL "http://nvidia.com/"

GST_PLUGIN_DEFINE(
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    nvdsgst_videoprep,
    PACKAGE_DESCRIPTION,
    hm::videoprep::videoprep_init,
    "7.1",
    PACKAGE_LICENSE,
    PACKAGE_NAME,
    PACKAGE_URL)
