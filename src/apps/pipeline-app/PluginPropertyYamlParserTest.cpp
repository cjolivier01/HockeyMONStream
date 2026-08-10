#include "hstream/src/apps/apps-common/deepstream_dsfieldmask.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <iostream>
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
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);

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
  private-properties:
    runtime-output-max-width: 3840
    runtime-output-max-height: 2160
    no-crop: false
hmstitcher:
  enable: true
  plugin-type: hmstitcher
  post-stitch-rotate-degrees: 2.5
  ui-preview: 1
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
)yaml");

  HmPlayCropperConfig playcropper{};
  if (!parse_hmplaycropper_yaml(&playcropper, config["hmplaycropper"], "/tmp", true)) {
    std::cerr << "parse_hmplaycropper_yaml failed\n";
    return 1;
  }
  if (!expect_property(playcropper.plugin_properties, "output-width", "1920") ||
      !expect_property(playcropper.plugin_properties, "silent", "true") ||
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
  if (std::abs(stitcher.post_stitch_rotate_degrees - 2.5) > 1e-6 || !stitcher.ui_preview) {
    std::cerr << "Expected post-stitch rotation and UI preview settings to parse into HmStitcherConfig\n";
    return 1;
  }

  NvDsDsPlayTrackerConfig playtracker{};
  if (!parse_dsplaytracker_yaml(&playtracker, config["ds-playtracker"], "/tmp")) {
    std::cerr << "parse_dsplaytracker_yaml failed\n";
    return 1;
  }
  if (!expect_property(playtracker.plugin_properties, "source-id", "2") ||
      !expect_property(playtracker.private_properties, "show", "true")) {
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

  return 0;
}
