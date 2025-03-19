#include "videoprep_plugins.h"
#include "gstvideoprep.h"

#include "hstream/src/gst-plugins/gst-videoprep/playcropper/playcropper.h"
#include "hstream/src/gst-plugins/gst-videoprep/playtracker/playtracker.h"
#include "hstream/src/gst-plugins/gst-videoprep/stitcher/stitcher.h"

namespace hm {
namespace videoprep {

// GST_DEBUG_CATEGORY_STATIC(gst_videoprep_debug);
GST_DEBUG_CATEGORY_STATIC(gst_playcropper_debug);
GST_DEBUG_CATEGORY_STATIC(gst_stitcher_debug);

IDSCustomLibrary* VideoPrepLibrary_Factory::CreateCustomAlgoCtx(
    std::string libName,
    GObject* object,
    int gpu_id,
    size_t batch_size) {
  if (libName == "playcropper") {
    return new playcropper::PlayCropperPriv(gpu_id, batch_size);
  }
  if (libName == "playtracker") {
    return new playtracker::PlayTrackerPriv(gpu_id, batch_size);
  }
  if (libName == "hmstitcher") {
    return new stitcher::StitcherPriv(gpu_id, batch_size);
  }
  return DSCustomLibrary_Factory::CreateCustomAlgoCtx(libName, object);
}

/* entry point to initialize the plug-in
 * initialize the plug-in itself
 * register the element factories and other features
 */
static gboolean videoprep_init(GstPlugin* plugin) {
  /* debug category for filtering log messages
   *
   * exchange the string 'Template videoprep' with your description
   */
  // GST_DEBUG_CATEGORY_INIT(gst_videoprep_debug, "videoprep", 0, "videoprep");
  GST_DEBUG_CATEGORY_INIT(gst_playcropper_debug, "playcropper", 0, "playcropper");
  //GST_DEBUG_CATEGORY_INIT(gst_playcropper_debug, "playtracker", 0, "playtracker");
  GST_DEBUG_CATEGORY_INIT(gst_stitcher_debug, "hmstitcher", 0, "hmstitcher");

  gboolean result = false;
  // gboolean result = gst_element_register(plugin, "videoprep", GST_RANK_NONE, GST_TYPE_VIDEOPREP);
  result |= gst_element_register(plugin, "playcropper", GST_RANK_NONE, GST_TYPE_VIDEOPREP);
  //result |= gst_element_register(plugin, "playtracker", GST_RANK_NONE, GST_TYPE_VIDEOPREP);
  result |= gst_element_register(plugin, "hmstitcher", GST_RANK_NONE, GST_TYPE_VIDEOPREP);
  return result;
}
} // namespace videoprep
} // namespace hm

#ifndef PACKAGE
#define PACKAGE "videoprep"
#endif

#define PACKAGE_DESCRIPTION "Gstreamer plugin to do stuff"
#define PACKAGE_LICENSE "Proprietary"
#define PACKAGE_NAME "GStreamer Stuff Plugin"
#define PACKAGE_URL "http://ghute.fahrt/"

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
