#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <gst/gst.h>

#include <cmath>
#include <cstdlib>
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
              {"require-existing-mask", G_TYPE_BOOLEAN, true},
              {"raise-bbox-center-by-height-ratio", G_TYPE_FLOAT, true},
              {"lower-bbox-bottom-by-height-ratio", G_TYPE_FLOAT, true},
              {"left-bbox-by-half-width-ratio", G_TYPE_FLOAT, true},
              {"right-bbox-by-half-width-ratio", G_TYPE_FLOAT, true},
              {"mask-top-inset", G_TYPE_INT, true},
              {"mask-bottom-inset", G_TYPE_INT, true},
              {"mask-left-inset", G_TYPE_INT, true},
              {"mask-right-inset", G_TYPE_INT, true},
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
              {"require-existing-mask", "true"},
              {"raise-bbox-center-by-height-ratio", "-0.1"},
              {"lower-bbox-bottom-by-height-ratio", "0.1"},
              {"left-bbox-by-half-width-ratio", "0.4"},
              {"right-bbox-by-half-width-ratio", "-0.3"},
              {"mask-top-inset", "25"},
              {"mask-bottom-inset", "-35"},
              {"mask-left-inset", "40"},
              {"mask-right-inset", "-20"},
          })) {
    gst_object_unref(element);
    return 1;
  }

  guint unique_id = 0;
  for (const char* property :
       {"raise-bbox-center-by-height-ratio",
        "lower-bbox-bottom-by-height-ratio",
        "left-bbox-by-half-width-ratio",
        "right-bbox-by-half-width-ratio",
        "mask-top-inset",
        "mask-bottom-inset",
        "mask-left-inset",
        "mask-right-inset"}) {
    const auto* spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property);
    if (!spec || !(spec->flags & GST_PARAM_MUTABLE_PLAYING)) {
      std::cerr << property << " must be writable while playing\n";
      return 1;
    }
  }
  gfloat raise_center_ratio = 0.0F;
  gfloat lower_bottom_ratio = 0.0F;
  gchar* detection_mask = nullptr;
  gboolean require_existing = FALSE;
  g_object_get(
      G_OBJECT(element),
      "unique-id",
      &unique_id,
      "detection-mask",
      &detection_mask,
      "require-existing-mask",
      &require_existing,
      "raise-bbox-center-by-height-ratio",
      &raise_center_ratio,
      "lower-bbox-bottom-by-height-ratio",
      &lower_bottom_ratio,
      NULL);
  const bool ok = unique_id == 21 && require_existing && detection_mask &&
      std::string(detection_mask) == "/tmp/mask.png" && std::abs(raise_center_ratio + 0.1F) < 0.0001F &&
      std::abs(lower_bottom_ratio - 0.1F) < 0.0001F;
  g_free(detection_mask);
  gst_object_unref(element);

  if (!ok) {
    std::cerr << "dsfieldmask property roundtrip failed\n";
    return 1;
  }
  std::_Exit(0);
}
