#include "PlayerFrameScanSettings.h"
#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <iostream>

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  if (!hm::gst::test::load_plugin_from_runfiles("src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so"))
    return 1;
  GstElement* filter = gst_element_factory_make("dsfieldmask", nullptr);
  if (!filter)
    return 1;
  const auto original = hm::pipeline::PlayerFrameScanMaskSettings(filter);
  bool passed = true;
  for (const char* property : {"mask-top-inset", "mask-bottom-inset", "mask-left-inset", "mask-right-inset"}) {
    for (int value : {-37, 41}) {
      g_object_set(filter, property, value, nullptr);
      if (hm::pipeline::PlayerFrameScanMaskSettings(filter) == original) {
        std::cerr << property << " was omitted from scan provenance\n";
        passed = false;
      }
    }
    g_object_set(filter, property, 0, nullptr);
  }
  for (const char* property :
       {"raise-bbox-center-by-height-ratio",
        "lower-bbox-bottom-by-height-ratio",
        "left-bbox-by-half-width-ratio",
        "right-bbox-by-half-width-ratio"}) {
    gfloat previous = 0;
    g_object_get(filter, property, &previous, nullptr);
    g_object_set(filter, property, -0.375, nullptr);
    if (hm::pipeline::PlayerFrameScanMaskSettings(filter) == original) {
      std::cerr << property << " was omitted from scan provenance\n";
      passed = false;
    }
    g_object_set(filter, property, static_cast<double>(previous), nullptr);
  }
  g_object_set(filter, "require-existing-mask", TRUE, nullptr);
  passed = passed && hm::pipeline::PlayerFrameScanMaskSettings(filter) != original;
  g_object_set(filter, "require-existing-mask", FALSE, nullptr);
  passed = passed && hm::pipeline::PlayerFrameScanMaskSettings(filter) == original;
  gst_object_unref(filter);
  return passed ? 0 : 1;
}
