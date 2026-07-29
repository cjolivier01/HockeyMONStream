#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <gst/gst.h>

#include <cmath>
#include <iostream>

namespace {

bool expect_videoprep_factory(const char* factory_name) {
  return hm::gst::test::expect_element_contract(
      factory_name,
      {
          {"silent", G_TYPE_BOOLEAN, true},
          {"gpu-id", G_TYPE_UINT, true},
          {"source-id", G_TYPE_UINT, true},
          {"num-output-buffers", G_TYPE_UINT, true},
          {"num-batch-buffers", G_TYPE_UINT, true},
          {"output-width", G_TYPE_UINT, true},
          {"output-height", G_TYPE_UINT, true},
          {"config-file", G_TYPE_STRING, true},
          {"plugin-type", G_TYPE_STRING, true},
          {"plugin-private-config", G_TYPE_STRING, true},
          {"post-stitch-rotate-degrees", G_TYPE_DOUBLE, true},
          {"fixed-edge-rotation-angle", G_TYPE_DOUBLE, true},
          {"dynamic-acceleration-scaling", G_TYPE_DOUBLE, true},
          {"last-property-set-ok", G_TYPE_BOOLEAN, false},
      },
      {
          {"sink", GST_PAD_SINK, GST_PAD_ALWAYS},
          {"src", GST_PAD_SRC, GST_PAD_ALWAYS},
      });
}

} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);

  if (!hm::gst::test::load_plugin_from_runfiles("src/gst-plugins/gst-videoprep/libnvdsgst_videoprep.so")) {
    return 1;
  }

  for (const char* factory : {"videoprep", "playcropper", "vpplaytracker", "hmstitcher"}) {
    if (!expect_videoprep_factory(factory)) {
      return 1;
    }
  }

  GstElement* element = gst_element_factory_make("playcropper", nullptr);
  if (!element) {
    std::cerr << "Could not create playcropper\n";
    return 1;
  }
  if (!hm::gst::test::apply_and_expect_properties(
          element,
          {
              {"silent", "true"},
              {"source-id", "3"},
              {"output-width", "1920"},
              {"output-height", "1080"},
              {"plugin-type", "playcropper"},
              {"plugin-private-config", "show=1;runtime-output-max-width=3840"},
              {"fixed-edge-rotation-angle", "12.5"},
              {"dynamic-acceleration-scaling", "1.25"},
          })) {
    gst_object_unref(element);
    return 1;
  }

  gboolean silent = FALSE;
  guint source_id = 0;
  guint output_width = 0;
  gchar* plugin_type = nullptr;
  gchar* private_config = nullptr;
  gdouble fixed_edge_rotation_angle = 0.0;
  gdouble dynamic_acceleration_scaling = 0.0;
  gboolean last_property_set_ok = FALSE;
  g_object_get(
      G_OBJECT(element),
      "silent",
      &silent,
      "source-id",
      &source_id,
      "output-width",
      &output_width,
      "plugin-type",
      &plugin_type,
      "plugin-private-config",
      &private_config,
      "fixed-edge-rotation-angle",
      &fixed_edge_rotation_angle,
      "dynamic-acceleration-scaling",
      &dynamic_acceleration_scaling,
      "last-property-set-ok",
      &last_property_set_ok,
      NULL);

  const bool ok = silent == TRUE && source_id == 3 && output_width == 1920 && plugin_type &&
      std::string(plugin_type) == "playcropper" && private_config &&
      std::string(private_config) == "show=1;runtime-output-max-width=3840" &&
      std::abs(fixed_edge_rotation_angle - 12.5) < 1e-6 &&
      std::abs(dynamic_acceleration_scaling - 1.25) < 1e-6 && last_property_set_ok == TRUE;
  g_free(plugin_type);
  g_free(private_config);
  gst_object_unref(element);

  if (!ok) {
    std::cerr << "videoprep property roundtrip failed\n";
    return 1;
  }
  return 0;
}
