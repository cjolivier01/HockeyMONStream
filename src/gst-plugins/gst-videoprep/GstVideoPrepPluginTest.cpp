#include "hstream/src/gst-plugins/testutils/GstPluginTestHarness.h"

#include <gst/base/gstbasetransform.h>
#include <gst/gst.h>

#include <cmath>
#include <filesystem>
#include <fstream>
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
          {"fixed-edge-rotation-angle-left", G_TYPE_DOUBLE, true},
          {"fixed-edge-rotation-angle-right", G_TYPE_DOUBLE, true},
          {"dynamic-acceleration-scaling", G_TYPE_DOUBLE, true},
          {"preview-overlay-flags", G_TYPE_UINT, true},
          {"high-bit-depth", G_TYPE_BOOLEAN, true},
          {"shadow-lift", G_TYPE_DOUBLE, true},
          {"shadow-lift-black-point", G_TYPE_BOOLEAN, true},
          {"runtime-tuning-config-file", G_TYPE_STRING, false},
          {"last-property-set-ok", G_TYPE_BOOLEAN, false},
      },
      {
          {"sink", GST_PAD_SINK, GST_PAD_ALWAYS},
          {"src", GST_PAD_SRC, GST_PAD_ALWAYS},
      });
}

GstCaps* make_nvmm_rgba_caps(int width, int height) {
  GstCaps* caps = gst_caps_new_simple(
      "video/x-raw",
      "format",
      G_TYPE_STRING,
      "RGBA",
      "width",
      G_TYPE_INT,
      width,
      "height",
      G_TYPE_INT,
      height,
      "framerate",
      GST_TYPE_FRACTION,
      30,
      1,
      nullptr);
  gst_caps_set_features(caps, 0, gst_caps_features_new("memory:NVMM", nullptr));
  return caps;
}

bool expect_reverse_fixate_preserves_input_dimensions() {
  constexpr int kInputWidth = 320;
  constexpr int kInputHeight = 180;
  constexpr int kOutputWidth = 777;
  constexpr int kOutputHeight = 333;
  GstElement* element = gst_element_factory_make("playcropper", nullptr);
  if (!element) {
    std::cerr << "Could not create playcropper for reverse fixate test\n";
    return false;
  }
  g_object_set(
      element, "output-width", kOutputWidth, "output-height", kOutputHeight, "plugin-type", "playcropper", nullptr);

  GstBaseTransformClass* transform_class = GST_BASE_TRANSFORM_GET_CLASS(element);
  GstCaps* input_caps = make_nvmm_rgba_caps(kInputWidth, kInputHeight);
  GstCaps* output_caps = make_nvmm_rgba_caps(kOutputWidth, kOutputHeight);
  bool ok = transform_class->accept_caps(GST_BASE_TRANSFORM(element), GST_PAD_SINK, input_caps);
  GstCaps* candidate_input_caps = gst_caps_copy(input_caps);
  GstCaps* fixated_input_caps =
      transform_class->fixate_caps(GST_BASE_TRANSFORM(element), GST_PAD_SRC, output_caps, candidate_input_caps);

  int width = 0;
  int height = 0;
  const GstStructure* structure = fixated_input_caps ? gst_caps_get_structure(fixated_input_caps, 0) : nullptr;
  ok = ok && structure && gst_structure_get_int(structure, "width", &width) &&
      gst_structure_get_int(structure, "height", &height) && width == kInputWidth && height == kInputHeight;
  if (!ok) {
    std::cerr << "videoprep reverse fixate replaced input dimensions with output dimensions\n";
  }

  if (fixated_input_caps)
    gst_caps_unref(fixated_input_caps);
  gst_caps_unref(output_caps);
  gst_caps_unref(input_caps);
  gst_object_unref(element);
  return ok;
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
  if (!expect_reverse_fixate_preserves_input_dimensions()) {
    return 1;
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
              {"fixed-edge-rotation-angle-left", "25.0"},
              {"fixed-edge-rotation-angle-right", "75.0"},
              {"dynamic-acceleration-scaling", "1.25"},
              {"high-bit-depth", "true"},
              {"shadow-lift", "42.5"},
              {"shadow-lift-black-point", "true"},
              {"exposure", "1.0"},
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
  gdouble fixed_edge_rotation_angle_left = 0.0;
  gdouble fixed_edge_rotation_angle_right = 0.0;
  gdouble dynamic_acceleration_scaling = 0.0;
  gboolean high_bit_depth = FALSE;
  gdouble shadow_lift = 0.0;
  gboolean shadow_lift_black_point = FALSE;
  gdouble exposure = 0.0;
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
      "fixed-edge-rotation-angle-left",
      &fixed_edge_rotation_angle_left,
      "fixed-edge-rotation-angle-right",
      &fixed_edge_rotation_angle_right,
      "dynamic-acceleration-scaling",
      &dynamic_acceleration_scaling,
      "high-bit-depth",
      &high_bit_depth,
      "shadow-lift",
      &shadow_lift,
      "shadow-lift-black-point",
      &shadow_lift_black_point,
      "exposure",
      &exposure,
      "last-property-set-ok",
      &last_property_set_ok,
      NULL);

  const bool ok = silent == TRUE && source_id == 3 && output_width == 1920 && plugin_type &&
      std::string(plugin_type) == "playcropper" && private_config &&
      std::string(private_config) == "show=1;runtime-output-max-width=3840" &&
      std::abs(fixed_edge_rotation_angle - 12.5) < 1e-6 && std::abs(fixed_edge_rotation_angle_left - 25.0) < 1e-6 &&
      std::abs(fixed_edge_rotation_angle_right - 75.0) < 1e-6 && std::abs(dynamic_acceleration_scaling - 1.25) < 1e-6 &&
      high_bit_depth == TRUE && std::abs(shadow_lift - 42.5) < 1e-6 && shadow_lift_black_point == TRUE &&
      std::abs(exposure - 1.0) < 1e-6 && last_property_set_ok == TRUE;
  GParamSpec* high_bit_depth_spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "high-bit-depth");
  const bool high_bit_depth_is_restart_only = high_bit_depth_spec &&
      (high_bit_depth_spec->flags & GST_PARAM_MUTABLE_READY) != 0 &&
      (high_bit_depth_spec->flags & GST_PARAM_MUTABLE_PLAYING) == 0;
  GstPadTemplate* sink_template = gst_element_class_get_pad_template(GST_ELEMENT_GET_CLASS(element), "sink");
  GstCaps* sink_caps = sink_template ? gst_pad_template_get_caps(sink_template) : nullptr;
  GstCaps* rgb10_caps = gst_caps_from_string("video/x-raw(memory:NVMM),format=RGB10A2_LE");
  const bool sink_accepts_rgb10 = sink_caps && rgb10_caps && gst_caps_can_intersect(sink_caps, rgb10_caps);
  if (rgb10_caps) {
    gst_caps_unref(rgb10_caps);
  }
  if (sink_caps) {
    gst_caps_unref(sink_caps);
  }
  GParamSpec* shadow_black_point_spec =
      g_object_class_find_property(G_OBJECT_GET_CLASS(element), "shadow-lift-black-point");
  const bool black_point_mutable_while_playing =
      shadow_black_point_spec && (shadow_black_point_spec->flags & GST_PARAM_MUTABLE_PLAYING) != 0;
  GParamSpec* exposure_spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), "exposure");
  const bool exposure_mutable_while_playing = exposure_spec && (exposure_spec->flags & GST_PARAM_MUTABLE_PLAYING) != 0;
  const bool invalid_black_point_rejected =
      !hm::gst::apply_plugin_properties(G_OBJECT(element), {{"shadow-lift-black-point", "2"}});
  gboolean black_point_after_invalid = FALSE;
  g_object_get(G_OBJECT(element), "shadow-lift-black-point", &black_point_after_invalid, NULL);
  const bool invalid_exposure_rejected = !hm::gst::apply_plugin_properties(G_OBJECT(element), {{"exposure", "1.31"}});
  gdouble exposure_after_invalid = 0.0;
  g_object_get(G_OBJECT(element), "exposure", &exposure_after_invalid, NULL);
  const bool invalid_high_bit_depth_rejected =
      !hm::gst::apply_plugin_properties(G_OBJECT(element), {{"high-bit-depth", "2"}});
  gboolean high_bit_depth_after_invalid = FALSE;
  g_object_get(G_OBJECT(element), "high-bit-depth", &high_bit_depth_after_invalid, NULL);
  g_free(plugin_type);
  g_free(private_config);
  gst_object_unref(element);

  if (!ok || !high_bit_depth_is_restart_only || !sink_accepts_rgb10 || !black_point_mutable_while_playing ||
      !exposure_mutable_while_playing || !invalid_black_point_rejected || black_point_after_invalid != TRUE ||
      !invalid_exposure_rejected || std::abs(exposure_after_invalid - 1.0) > 1e-6 || !invalid_high_bit_depth_rejected ||
      high_bit_depth_after_invalid != TRUE) {
    std::cerr << "videoprep property roundtrip failed\n";
    return 1;
  }

  const std::filesystem::path valid_base = std::filesystem::temp_directory_path() / "hstream-vpplaytracker-base.yaml";
  const std::filesystem::path invalid_runtime =
      std::filesystem::temp_directory_path() / "hstream-invalid-vpplaytracker-runtime.yaml";
  {
    std::ofstream out(valid_base);
    out << "play-tracker:\n  live-boxes:\n    - name: current_roi\n    - name: current_roi_aspect\n";
  }
  {
    std::ofstream out(invalid_runtime);
    out << "play-tracker:\n  live-boxes: invalid\n";
  }
  GstElement* tracker = gst_element_factory_make("vpplaytracker", nullptr);
  if (!tracker) {
    std::cerr << "Could not create vpplaytracker\n";
    return 1;
  }
  g_object_set(
      G_OBJECT(tracker),
      "plugin-type",
      "vpplaytracker",
      "config-file",
      valid_base.c_str(),
      "preview-overlay-flags",
      5U,
      nullptr);
  guint preview_overlay_flags = 0;
  g_object_get(G_OBJECT(tracker), "preview-overlay-flags", &preview_overlay_flags, nullptr);
  GParamSpec* preview_spec = g_object_class_find_property(G_OBJECT_GET_CLASS(tracker), "preview-overlay-flags");
  const GstStateChangeReturn state_result = gst_element_set_state(tracker, GST_STATE_READY);
  gboolean invalid_accepted = TRUE;
  if (state_result == GST_STATE_CHANGE_SUCCESS) {
    g_object_set(G_OBJECT(tracker), "runtime-tuning-config-file", invalid_runtime.c_str(), nullptr);
    g_object_get(G_OBJECT(tracker), "last-property-set-ok", &invalid_accepted, nullptr);
  }
  gst_element_set_state(tracker, GST_STATE_NULL);
  gst_object_unref(tracker);
  std::filesystem::remove(valid_base);
  std::filesystem::remove(invalid_runtime);
  if (state_result != GST_STATE_CHANGE_SUCCESS || invalid_accepted || preview_overlay_flags != 5 || !preview_spec ||
      (preview_spec->flags & GST_PARAM_MUTABLE_PLAYING) == 0) {
    std::cerr << "vpplaytracker production property path did not reject invalid runtime tuning\n";
    return 1;
  }
  return 0;
}
