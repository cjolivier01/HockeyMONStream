#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <gst/gst.h>

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  if (!hm::gst::test::load_plugin_from_runfiles("src/gst-plugins/gst-playtracker/libgstplaytracker.so")) {
    return 1;
  }
  if (!hm::gst::test::expect_element_contract(
          "playtracker",
          {
              {"unique-id", G_TYPE_UINT, true},
              {"gpu-id", G_TYPE_UINT, true},
              {"draw", G_TYPE_BOOLEAN, true},
              {"config-file", G_TYPE_STRING, true},
          },
          {
              {"sink", GST_PAD_SINK, GST_PAD_ALWAYS},
              {"src", GST_PAD_SRC, GST_PAD_ALWAYS},
          })) {
    return 1;
  }

  GstElement* element = gst_element_factory_make("playtracker", nullptr);
  if (!element) {
    std::cerr << "Could not create playtracker\n";
    return 1;
  }
  if (!hm::gst::test::apply_and_expect_properties(
          element,
          {
              {"unique-id", "22"},
              {"draw", "true"},
              {"config-file", "/tmp/play_tracker.yaml"},
          })) {
    gst_object_unref(element);
    return 1;
  }

  guint unique_id = 0;
  gboolean draw = FALSE;
  gchar* config_file = nullptr;
  g_object_get(G_OBJECT(element), "unique-id", &unique_id, "draw", &draw, "config-file", &config_file, NULL);
  const bool ok =
      unique_id == 22 && draw == TRUE && config_file && std::string(config_file) == "/tmp/play_tracker.yaml";
  g_free(config_file);
  gst_object_unref(element);

  if (!ok) {
    std::cerr << "playtracker property roundtrip failed\n";
    return 1;
  }

  std::_Exit(0);
}
