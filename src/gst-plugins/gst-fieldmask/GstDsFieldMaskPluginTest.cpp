#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <gst/gst.h>

#include <iostream>

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  if (!hm::gst::test::load_plugin_from_runfiles("src/gst-plugins/gst-fieldmask/libnvdsgst_dsfieldmask.so")) {
    return 1;
  }
  if (!hm::gst::test::expect_element_contract(
          "dsfieldmask",
          {
              {"unique-id", G_TYPE_UINT, true},
              {"gpu-id", G_TYPE_UINT, true},
              {"detection-mask", G_TYPE_STRING, true},
          },
          {
              {"sink", GST_PAD_SINK, GST_PAD_ALWAYS},
              {"src", GST_PAD_SRC, GST_PAD_ALWAYS},
          })) {
    return 1;
  }

  GstElement* element = gst_element_factory_make("dsfieldmask", nullptr);
  if (!element) {
    std::cerr << "Could not create dsfieldmask\n";
    return 1;
  }
  if (!hm::gst::test::apply_and_expect_properties(
          element,
          {
              {"unique-id", "21"},
              {"gpu-id", "0"},
              {"detection-mask", "/tmp/mask.png"},
          })) {
    gst_object_unref(element);
    return 1;
  }

  guint unique_id = 0;
  gchar* detection_mask = nullptr;
  g_object_get(G_OBJECT(element), "unique-id", &unique_id, "detection-mask", &detection_mask, NULL);
  const bool ok = unique_id == 21 && detection_mask && std::string(detection_mask) == "/tmp/mask.png";
  g_free(detection_mask);
  gst_object_unref(element);

  if (!ok) {
    std::cerr << "dsfieldmask property roundtrip failed\n";
    return 1;
  }
  return 0;
}
