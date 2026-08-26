#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"

#include "deepstream_app.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <iostream>
#include <memory>
#include <string>

GST_DEBUG_CATEGORY(NVDS_APP);

gboolean parse_dsplaytracker_yaml(
    NvDsDsPlayTrackerConfig* config,
    const YAML::Node& yaml_node,
    const std::string& config_dir);
gboolean parse_hmplaycropper_yaml(
    HmPlayCropperConfig* config,
    const YAML::Node& yaml_node,
    const std::string& config_dir,
    bool quiet = false);
gboolean parse_hmstitcher_yaml(HmStitcherConfig* config, const YAML::Node& yaml_node, const std::string& config_dir);

namespace {

bool expect_property(const hm::gst::PluginProperties& properties, const std::string& name, const std::string& value) {
  for (const auto& property : properties) {
    if (property.name == name && property.value == value) {
      return true;
    }
  }
  std::cerr << "Missing property " << name << "=" << value << '\n';
  return false;
}

} // namespace

int main() {
  gst_init(nullptr, nullptr);
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

  auto terminal_outcome = std::make_unique<AppCtx>();
  if (!mark_terminal_source_error_failure(terminal_outcome.get()) || terminal_outcome->return_value == 0) {
    std::cerr << "A terminal source error without observed EOS did not set a failure result\n";
    return 1;
  }
  terminal_outcome->return_value = 0;
  terminal_outcome->eos_received = TRUE;
  if (mark_terminal_source_error_failure(terminal_outcome.get()) || terminal_outcome->return_value != 0) {
    std::cerr << "A positively observed EOS was treated as a terminal source failure\n";
    return 1;
  }

  const YAML::Node config = YAML::Load(R"yaml(
hmplaycropper:
  enable: true
  gpu-id: 0
  plugin-type: playcropper
  config-file: config.yaml
  fixed-edge-rotation-angle-left: 25.0
  fixed-edge-rotation-angle-right: 75.0
  properties:
    output-width: 1920
    output-height: 1080
    silent: true
    shadow-lift: 35
    shadow-lift-black-point: true
    exposure: 0.6
  private-properties:
    runtime-output-max-width: 3840
    runtime-output-max-height: 2160
    no-crop: false
hmstitcher:
  enable: true
  plugin-type: hmstitcher
  post-stitch-rotate-degrees: 2.5
  ui-preview: 1
  calibration-sample-span-ns: 120000000000
  properties:
    num-output-buffers: 6
  private-properties:
    one-pass-mode: true
    configure-only: false
ds-playtracker:
  enable: true
  config-file: tracker.yaml
  fixed-edge-rotation-angle-left: 25.0
  fixed-edge-rotation-angle-right: 75.0
  properties:
    source-id: 2
  private-properties:
    show: true
    telemetry-csv-dir: /tmp/hm-game
)yaml");

  HmPlayCropperConfig playcropper{};
  if (!parse_hmplaycropper_yaml(&playcropper, config["hmplaycropper"], "/tmp", true)) {
    std::cerr << "parse_hmplaycropper_yaml failed\n";
    return 1;
  }
  if (!expect_property(playcropper.plugin_properties, "output-width", "1920") ||
      !expect_property(playcropper.plugin_properties, "silent", "true") ||
      !expect_property(playcropper.plugin_properties, "shadow-lift", "35") ||
      !expect_property(playcropper.plugin_properties, "shadow-lift-black-point", "true") ||
      !expect_property(playcropper.plugin_properties, "exposure", "0.6") ||
      !expect_property(playcropper.private_properties, "runtime-output-max-width", "3840") ||
      !expect_property(playcropper.private_properties, "no-crop", "false")) {
    return 1;
  }
  if (!playcropper.fixed_edge_rotation_angle_left_set || !playcropper.fixed_edge_rotation_angle_right_set ||
      std::abs(playcropper.fixed_edge_rotation_angle_left - 25.0) > 1e-6 ||
      std::abs(playcropper.fixed_edge_rotation_angle_right - 75.0) > 1e-6) {
    std::cerr << "Expected independent playcropper fixed-edge rotation angles\n";
    return 1;
  }

  HmStitcherConfig stitcher{};
  if (!parse_hmstitcher_yaml(&stitcher, config["hmstitcher"], "/tmp")) {
    std::cerr << "parse_hmstitcher_yaml failed\n";
    return 1;
  }
  if (!expect_property(stitcher.plugin_properties, "num-output-buffers", "6") ||
      !expect_property(stitcher.private_properties, "one-pass-mode", "true")) {
    return 1;
  }
  if (std::abs(stitcher.post_stitch_rotate_degrees - 2.5) > 1e-6 || !stitcher.ui_preview ||
      stitcher.calibration_sample_span_ns != 120000000000UL) {
    std::cerr << "Expected post-stitch rotation, UI preview, and calibration sample span settings to parse into "
                 "HmStitcherConfig\n";
    return 1;
  }

  NvDsDsPlayTrackerConfig playtracker{};
  if (!parse_dsplaytracker_yaml(&playtracker, config["ds-playtracker"], "/tmp")) {
    std::cerr << "parse_dsplaytracker_yaml failed\n";
    return 1;
  }
  if (!expect_property(playtracker.plugin_properties, "source-id", "2") ||
      !expect_property(playtracker.private_properties, "show", "true") ||
      !expect_property(playtracker.private_properties, "telemetry-csv-dir", "/tmp/hm-game")) {
    return 1;
  }
  if (!playtracker.fixed_edge_rotation_angle_left_set || !playtracker.fixed_edge_rotation_angle_right_set ||
      std::abs(playtracker.fixed_edge_rotation_angle_left - 25.0) > 1e-6 ||
      std::abs(playtracker.fixed_edge_rotation_angle_right - 75.0) > 1e-6) {
    std::cerr << "Expected independent playtracker fixed-edge rotation angles\n";
    return 1;
  }

  NvDsDsPlayTrackerConfig malformed{};
  const YAML::Node bad_properties = YAML::Load(R"yaml(
enable: true
properties:
  - source-id=2
)yaml");
  if (parse_dsplaytracker_yaml(&malformed, bad_properties, "/tmp")) {
    std::cerr << "Expected malformed properties to fail parsing\n";
    return 1;
  }

  HmPlayCropperConfig malformed_private{};
  const YAML::Node bad_private_properties = YAML::Load(R"yaml(
enable: true
private-properties:
  runtime-output-max-width: 3840;extra=true
)yaml");
  if (parse_hmplaycropper_yaml(&malformed_private, bad_private_properties, "/tmp", true)) {
    std::cerr << "Expected malformed private-properties to fail parsing\n";
    return 1;
  }

  HmPlayCropperConfig bad_playcropper_numeric{};
  const YAML::Node bad_playcropper_fixed_edge = YAML::Load(R"yaml(
enable: true
fixed-edge-rotation-angle: abc
)yaml");
  if (parse_hmplaycropper_yaml(&bad_playcropper_numeric, bad_playcropper_fixed_edge, "/tmp", true)) {
    std::cerr << "Expected invalid hmplaycropper fixed-edge-rotation-angle to fail parsing\n";
    return 1;
  }

  NvDsDsPlayTrackerConfig bad_playtracker_fixed_edge{};
  const YAML::Node bad_playtracker_numeric = YAML::Load(R"yaml(
enable: true
fixed_edge_rotation_angle: .nan
)yaml");
  if (parse_dsplaytracker_yaml(&bad_playtracker_fixed_edge, bad_playtracker_numeric, "/tmp")) {
    std::cerr << "Expected invalid ds-playtracker fixed_edge_rotation_angle to fail parsing\n";
    return 1;
  }

  NvDsDsPlayTrackerConfig bad_playtracker_dynamic{};
  const YAML::Node bad_playtracker_dynamic_scaling = YAML::Load(R"yaml(
enable: true
dynamic-acceleration-scaling: .inf
)yaml");
  if (parse_dsplaytracker_yaml(&bad_playtracker_dynamic, bad_playtracker_dynamic_scaling, "/tmp")) {
    std::cerr << "Expected invalid ds-playtracker dynamic-acceleration-scaling to fail parsing\n";
    return 1;
  }

  HmStitcherConfig bad_stitcher_numeric{};
  const YAML::Node bad_stitcher_rotation = YAML::Load(R"yaml(
enable: true
post-stitch-rotate-degrees: nope
)yaml");
  if (parse_hmstitcher_yaml(&bad_stitcher_numeric, bad_stitcher_rotation, "/tmp")) {
    std::cerr << "Expected invalid post-stitch-rotate-degrees to fail parsing\n";
    return 1;
  }

  auto forged_stitcher_authority = std::make_unique<HmStitcherConfig>();
  const YAML::Node forged_stitcher_authority_yaml = YAML::Load(R"yaml(
enable: true
properties:
  plugin-private-config: calibration-run-generation=forged
)yaml");
  if (parse_hmstitcher_yaml(forged_stitcher_authority.get(), forged_stitcher_authority_yaml, "/tmp")) {
    std::cerr << "Expected public plugin-private-config to be rejected for hmstitcher\n";
    return 1;
  }

  HmStitcherConfig null_stitcher_rotation{};
  null_stitcher_rotation.post_stitch_rotate_degrees = 6.0F;
  const YAML::Node null_stitcher_rotation_yaml = YAML::Load(R"yaml(
post-stitch-rotate-degrees: null
)yaml");
  if (!parse_hmstitcher_yaml(&null_stitcher_rotation, null_stitcher_rotation_yaml, "/tmp") ||
      null_stitcher_rotation.post_stitch_rotate_degrees != 6.0F) {
    std::cerr << "Expected null post-stitch-rotate-degrees to behave as unset\n";
    return 1;
  }

  HmStitcherConfig underscored_null_stitcher_rotation{};
  underscored_null_stitcher_rotation.post_stitch_rotate_degrees = 7.0F;
  const YAML::Node underscored_null_stitcher_rotation_yaml = YAML::Load(R"yaml(
post_stitch_rotate_degrees: null
)yaml");
  if (!parse_hmstitcher_yaml(&underscored_null_stitcher_rotation, underscored_null_stitcher_rotation_yaml, "/tmp") ||
      underscored_null_stitcher_rotation.post_stitch_rotate_degrees != 7.0F) {
    std::cerr << "Expected null underscored post_stitch_rotate_degrees to behave as unset\n";
    return 1;
  }

  auto dashed_null_underscored_rotation = std::make_unique<HmStitcherConfig>();
  const YAML::Node dashed_null_underscored_rotation_yaml = YAML::Load(R"yaml(
post-stitch-rotate-degrees: null
post_stitch_rotate_degrees: 8.5
)yaml");
  if (!parse_hmstitcher_yaml(dashed_null_underscored_rotation.get(), dashed_null_underscored_rotation_yaml, "/tmp") ||
      dashed_null_underscored_rotation->post_stitch_rotate_degrees != 8.5F) {
    std::cerr << "Expected a non-null underscored rotation to follow a null dashed alias\n";
    return 1;
  }

  auto dashed_null_malformed_underscored_rotation = std::make_unique<HmStitcherConfig>();
  const YAML::Node dashed_null_malformed_underscored_rotation_yaml = YAML::Load(R"yaml(
post-stitch-rotate-degrees: null
post_stitch_rotate_degrees: invalid
)yaml");
  if (parse_hmstitcher_yaml(
          dashed_null_malformed_underscored_rotation.get(), dashed_null_malformed_underscored_rotation_yaml, "/tmp")) {
    std::cerr << "Expected a malformed active underscored rotation after a null dashed alias to fail\n";
    return 1;
  }

  auto parsed_default_converter = std::make_unique<NvDsConfig>();
  if (!parse_config_yaml(
          YAML::Load("application:\n  enable-perf-measurement: 0\n"), parsed_default_converter.get(), "/tmp") ||
      std::string(parsed_default_converter->video_converter) != hm::deepstream::kNvVideoConvertElement ||
      std::string(hm::deepstream::video_converter_element_name()) != hm::deepstream::kNvVideoConvertElement) {
    std::cerr << "application.video-converter did not default to nvvideoconvert\n";
    return 1;
  }

  auto parsed_dsx_converter = std::make_unique<NvDsConfig>();
  const YAML::Node dsx_converter_config = YAML::Load(R"yaml(
application:
  enable-perf-measurement: 0
  video-converter: dsxvideoconvert
)yaml");
  if (!parse_config_yaml(dsx_converter_config, parsed_dsx_converter.get(), "/tmp") ||
      std::string(parsed_dsx_converter->video_converter) != hm::deepstream::kDsxVideoConvertElement ||
      std::string(hm::deepstream::video_converter_element_name()) != hm::deepstream::kDsxVideoConvertElement) {
    std::cerr << "application.video-converter did not select dsxvideoconvert\n";
    return 1;
  }

  auto parsed_bad_converter = std::make_unique<NvDsConfig>();
  const YAML::Node bad_converter_config = YAML::Load(R"yaml(
application:
  enable-perf-measurement: 0
  video-converter: videoconvert
)yaml");
  if (parse_config_yaml(bad_converter_config, parsed_bad_converter.get(), "/tmp")) {
    std::cerr << "Expected invalid application.video-converter to fail parsing\n";
    return 1;
  }

  const YAML::Node uri_playlist_config = YAML::Load(R"yaml(
application:
  enable-perf-measurement: 0
source0:
  enable: 1
  type: 3
  num-sources: 1
  source-id: 0
  uri-list:
    - file:///tmp/left-0.mp4
    - file:///tmp/left-1.mp4
source1:
  enable: 1
  type: 3
  num-sources: 1
  source-id: 1
  uri: file:///tmp/right-0.mp4
)yaml");
  auto parsed_playlist = std::make_unique<NvDsConfig>();
  if (!parse_config_yaml(uri_playlist_config, parsed_playlist.get(), "/tmp") ||
      parsed_playlist->num_source_sub_bins != 2 ||
      parsed_playlist->multi_source_config[0].type != NV_DS_SOURCE_URI_MULTIPLE ||
      parsed_playlist->multi_source_config[1].type != NV_DS_SOURCE_URI_MULTIPLE) {
    std::cerr << "URI playlist parsing did not retain two logical URI_MULTIPLE cameras\n";
    return 1;
  }

  g_setenv("USE_NEW_NVSTREAMMUX", "yes", TRUE);
  auto playlist_sources = std::make_unique<NvDsSrcParentBin>();
  if (!create_multi_source_bin(
          parsed_playlist->num_source_sub_bins, parsed_playlist->multi_source_config, playlist_sources.get())) {
    std::cerr << "Could not construct parsed URI playlist sources\n";
    return 1;
  }
  GstElementFactory* playlist_mux_factory = gst_element_get_factory(playlist_sources->streammux);
  const gchar* playlist_mux_name =
      playlist_mux_factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(playlist_mux_factory)) : nullptr;
  if (g_strcmp0(playlist_mux_name, "hstreamlosslessmux") != 0) {
    std::cerr << "Parsed two-camera URI playlist selected " << (playlist_mux_name ? playlist_mux_name : "<none>")
              << " instead of hstreamlosslessmux\n";
    gst_object_unref(playlist_sources->bin);
    return 1;
  }
  gst_object_unref(playlist_sources->bin);

  const YAML::Node single_playback_config = YAML::Load(R"yaml(
application:
  enable-perf-measurement: 0
source0:
  enable: 1
  type: 3
  num-sources: 1
  source-id: 0
  uri: file:///tmp/stitched-output.mp4
)yaml");
  auto parsed_single = std::make_unique<NvDsConfig>();
  if (!parse_config_yaml(single_playback_config, parsed_single.get(), "/tmp") ||
      parsed_single->num_source_sub_bins != 1 ||
      parsed_single->multi_source_config[0].type != NV_DS_SOURCE_URI_MULTIPLE) {
    std::cerr << "Single stitched-output source did not retain URI_MULTIPLE playlist semantics\n";
    return 1;
  }
  auto single_sources = std::make_unique<NvDsSrcParentBin>();
  if (!create_multi_source_bin(1, parsed_single->multi_source_config, single_sources.get())) {
    std::cerr << "Could not construct parsed single stitched-output source\n";
    return 1;
  }
  GstElementFactory* single_mux_factory = gst_element_get_factory(single_sources->streammux);
  const gchar* single_mux_name =
      single_mux_factory ? gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(single_mux_factory)) : nullptr;
  if (single_sources->uri_playlist_exact_pairing_enabled || g_strcmp0(single_mux_name, "hstreamlosslessmux") == 0) {
    std::cerr << "Parsed single stitched-output source incorrectly enabled exact two-camera pairing\n";
    gst_object_unref(single_sources->bin);
    return 1;
  }
  gst_object_unref(single_sources->bin);

  return 0;
}
