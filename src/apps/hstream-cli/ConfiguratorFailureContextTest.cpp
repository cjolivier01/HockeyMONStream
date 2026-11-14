#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/CalibrationFailureProtocol.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "src/apps/hstream-cli/configurator.h"

#include <gst/gst.h>
#include <cstdlib>
#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);
namespace hm {
struct ConfiguratorTestAccess {
  static void freeze(Configurator& value, const YAML::Node& selected, const YAML::Node& defaults) {
    value.config_ = YAML::Clone(selected);
    value.lower_layer_config_ = YAML::Clone(defaults);
    value.explicit_value_ranks_.clear();
    value.stitching_calibration_required_ = true;
    value.stitching_calibration_start_stage_ = "rink-mask";
  }
  static absl::Status width(Configurator& value, const std::string& path, int rank) {
    value.explicit_value_ranks_[path] = rank;
    return value.map_stitch_max_output_width();
  }
  static void automatic_width(Configurator& value) {
    value.config_["stitching"]["max_output_width"] = YAML::Node(YAML::NodeType::Null);
  }
};
} // namespace hm
namespace {
void require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace
int main() {
  const auto baseline = hm::baseline_config::load();
  require(baseline.ok(), baseline.status().ToString());
  YAML::Node defaults = YAML::Clone(baseline->values);
  defaults["hstream_ui"].remove("stitching_calibration");
  defaults["stitching"]["control_point_limit"] = 1500;
  auto selected = YAML::Clone(defaults);
  selected["hstream_ui"]["stitching_calibration"]["control_points"] = 250;
  selected["pipeline"]["hmstitcher"]["calibration-frame-count"] = 7;
  auto camera = hm::stitching::read_stitch_camera_selection(selected);
  require(camera.ok(), camera.status().ToString());
  camera->input_start_times["private-recording.mp4"] = 1.25;
  hm::stitching::write_stitch_camera_selection(selected, *camera);
  // The emitter may not fall back to a newly read baseline or user preference.
  setenv("HM_CONFIG_ROOT", "/nonexistent-failure-context-baseline", 1);
  for (int variant = 0; variant < 4; ++variant) {
    auto document = YAML::Clone(selected);
    auto stitcher = document["pipeline"]["hmstitcher"];
    stitcher["properties"].remove("max-output-width");
    stitcher["private-properties"].remove("max-output-width");
    document["stitching"]["max_output_width"] = YAML::Node(YAML::NodeType::Null);
    const std::string alias = variant == 1 ? "pipeline.hmstitcher.private-properties.stitch_max_output_width"
                                           : "pipeline.hmstitcher.properties.max-output-width";
    if (variant == 1)
      stitcher["private-properties"]["stitch_max_output_width"] = 8192;
    else
      stitcher["properties"]["max-output-width"] = 4096;
    hm::Configurator config("context", "/nonexistent-config-root", -1);
    hm::ConfiguratorTestAccess::freeze(config, document, defaults);
    require(hm::ConfiguratorTestAccess::width(config, alias, 2).ok(), "Ranked width alias must resolve");
    if (variant >= 2) {
      if (variant == 2)
        require(config.apply_config_item("stitching.max_output_width", "3072").ok(), "Explicit width must apply");
      else
        hm::ConfiguratorTestAccess::automatic_width(config);
      require(
          hm::ConfiguratorTestAccess::width(config, "stitching.max_output_width", 3).ok(),
          "Canonical CLI width must resolve");
    }
    const auto before = YAML::Dump(config.config());
    const auto context = config.calibration_failure_context(false);
    require(context.ok(), context.status().ToString());
    const int expected = variant == 0 ? 4096 : variant == 1 ? 8192 : variant == 2 ? 3072 : 0;
    require(
        context->max_output_width == expected, "Context must report mapped native width including null/aliases/ranks");
    require(
        context->control_point_limit == 250 && context->default_control_point_limit == 1500 &&
            context->frame_count == 7,
        "Context must freeze actual selected values and effective defaults");
    require(
        context->camera_differs_from_defaults && context->camera.configuration == context->default_camera.configuration,
        "Full source-bound camera inequality must survive identical display summaries");
    require(
        context->calibration_required && context->calibration_start_stage == "rink-mask",
        "Pending native context lost");
    const auto record = hm::stitching::failure::ContextRecord(std::string(64, 'a'), std::string(32, 'b'), 1, *context);
    require(
        record.ok() && record->find("private-recording.mp4") == std::string::npos,
        "Context must not expose private camera source paths");
    require(config.accept_prepared_rink_mask().ok(), "Prepared mask must be accepted");
    const auto ready = config.calibration_failure_context(false);
    require(
        ready.ok() && !ready->calibration_required,
        "Emission after prepared-mask acceptance must report final playback state");
    require(before == YAML::Dump(config.config()), "Context projection must not mutate configuration");
  }
  std::cout << "Configurator frozen failure context passed\n";
}
