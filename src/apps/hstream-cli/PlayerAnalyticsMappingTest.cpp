#include "hstream/src/apps/apps-common/PlayerAnalyticsBin.h"
#include "src/apps/hstream-cli/StitchingCalibrationMode.h"
#include "src/apps/hstream-cli/configurator.h"

#include <cstdlib>
#include <iostream>

GST_DEBUG_CATEGORY(NVDS_APP);

namespace hm {
struct ConfiguratorTestAccess {
  static absl::Status Map(
      Configurator& configurator,
      const YAML::Node& config,
      const std::map<std::string, int>& ranks) {
    configurator.config_ = YAML::Clone(config);
    configurator.explicit_value_ranks_ = ranks;
    return configurator.map_common_config_keys();
  }
};
} // namespace hm

namespace {
void Require(bool value, const std::string& message) {
  if (!value) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
void Map(hm::Configurator& configurator, const YAML::Node& document, const std::map<std::string, int>& ranks = {}) {
  const auto status = hm::ConfiguratorTestAccess::Map(configurator, document, ranks);
  Require(status.ok(), status.ToString());
}

void Precedence(const YAML::Node& baseline) {
  for (const auto& [canonical, native] :
       {std::pair<const char*, const char*>{"plot_pose", "draw-pose"},
        {"plot_jersey_numbers", "draw-jerseys"},
        {"plot_actions", "draw-actions"}}) {
    for (const bool canonical_value : {false, true}) {
      for (const auto& ranks : {std::pair{-1, -1}, {1, 1}, {2, 1}, {1, 2}, {3, 2}, {3, 3}}) {
        auto document = YAML::Clone(baseline);
        document["pipeline"] = YAML::Node(YAML::NodeType::Map);
        document["plot"][canonical] = canonical_value;
        document["pipeline"]["player-analytics"][native] = !canonical_value;
        document["pipeline"]["player-analytics"]["retained"] = "native setting";
        hm::Configurator configurator("unused", "/not-opened", -1);
        std::map<std::string, int> explicit_ranks;
        if (ranks.first >= 0)
          explicit_ranks[std::string("plot.") + canonical] = ranks.first;
        if (ranks.second >= 0)
          explicit_ranks[std::string("pipeline.player-analytics.") + native] = ranks.second;
        Map(configurator, document, explicit_ranks);
        const auto analytics = configurator.config()["pipeline"]["player-analytics"];
        const auto scalar = analytics[native].as<std::string>();
        const bool selected = scalar == "1" || scalar == "true";
        Require(
            selected == (ranks.first > ranks.second ? canonical_value : !canonical_value),
            std::string("Wrong canonical/native precedence for ") + canonical);
        Require(
            analytics["retained"].as<std::string>() == "native setting", "Mapping changed unrelated native settings");
      }
    }
    auto document = YAML::Clone(baseline);
    document["pipeline"] = YAML::Node(YAML::NodeType::Map);
    document["plot"][canonical] = YAML::Node(YAML::NodeType::Null);
    document["pipeline"]["player-analytics"][native] = 1;
    hm::Configurator configurator("unused", "/not-opened", -1);
    Map(configurator,
        document,
        {{std::string("plot.") + canonical, 2}, {std::string("pipeline.player-analytics.") + native, 1}});
    Require(
        configurator.config()["pipeline"]["player-analytics"][native].as<int>() == 1,
        "Suppressed null canonical mapping erased inherited native drawing");
  }
}

void DrawingAndCalibration(const YAML::Node& baseline) {
  auto document = YAML::Clone(baseline);
  document["pipeline"] = YAML::Load(R"(
player-analytics:
  pose: {enable: 0, bundle: /proc/1/player-analytics-do-not-open, rate-hz: broken}
  jersey: {enable: 0, bundle: [invalid]}
  action: {enable: 0, bundle: /proc/1/player-analytics-do-not-open}
  gpu-id: invalid
  batch-size: invalid
)");
  for (const char* key : {"plot_pose", "plot_jersey_numbers", "plot_actions"})
    document["plot"][key] = true;
  hm::Configurator configurator("unused", "/not-opened", -1);
  Map(configurator, document);
  auto resolved = hm::gst::ResolvePlayerAnalyticsConfig(configurator.config()["pipeline"], "/not-opened", -1);
  Require(
      resolved.ok() && !resolved->analytics.enabled() && resolved->serialized.empty() &&
          resolved->analytics.drawing.pose && resolved->analytics.drawing.jersey &&
          resolved->analytics.drawing.action && resolved->analytics.drawing_layers() == 0,
      "Drawing preferences activated compute, validated disabled fields, or lost their independent settings");
  auto element = hm::gst::CreatePlayerAnalytics(*resolved, false);
  Require(element.ok() && *element == nullptr, "Drawing-only settings attempted model/plugin setup");

  for (int mode = 0; mode < 5; ++mode) {
    auto calibration = YAML::Clone(document);
    // Deliberately invalid enabled dependencies must disappear before parsing.
    calibration["pipeline"]["player-analytics"] = YAML::Load("action: {enable: true, bundle: [invalid]}");
    const auto pipeline = calibration["pipeline"];
    switch (mode) {
      case 0:
        hm::pipeline_internal::configure_stitching_calibration_pipeline(pipeline);
        break;
      case 1:
        hm::pipeline_internal::configure_stitching_player_scan_pipeline(pipeline);
        break;
      case 2:
        hm::pipeline_internal::configure_int8_sampling_pipeline(pipeline);
        break;
      case 3:
        hm::pipeline_internal::configure_rink_mask_preparation_pipeline(pipeline, true);
        break;
      case 4:
        hm::pipeline_internal::suppress_player_analytics(pipeline);
        break;
    }
    resolved = hm::gst::ResolvePlayerAnalyticsConfig(pipeline, "/not-opened", -1);
    Require(
        resolved.ok() && !resolved->analytics.enabled() && resolved->analytics.drawing_layers() == 0 &&
            resolved->serialized.empty(),
        "Calibration/scan/sampling suppression left enabled analytics");
    element = hm::gst::CreatePlayerAnalytics(*resolved, false);
    Require(element.ok() && !*element, "Suppressed calibration requested an analytics factory");
  }

  hm::player_analytics::Config config;
  config.drawing = {true, true, true};
  Require(config.drawing_layers() == 0 && !config.enabled(), "Drawing-only config has GPU layers");
  config.pose.enabled = true;
  Require(config.drawing_layers() == hm::player_analytics::kDrawPose, "Pose drawing gate leaked another layer");
  config.jersey.enabled = true;
  Require(
      config.drawing_layers() == (hm::player_analytics::kDrawPose | hm::player_analytics::kDrawJerseys),
      "Jersey drawing gate is not independent");
  config.action.enabled = true;
  config.drawing.pose = config.drawing.jersey = false;
  Require(config.drawing_layers() == hm::player_analytics::kDrawActions, "Action drawing gate is not independent");
  config.drawing.action = false;
  Require(config.drawing_layers() == 0 && config.enabled(), "Drawing off disabled requested compute");
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  Require(argc == 2, "Usage: player_analytics_mapping_test BASELINE_YAML");
  const auto baseline = YAML::LoadFile(argv[1]);
  Precedence(baseline);
  DrawingAndCalibration(baseline);
  std::cout << "Actual Configurator drawing precedence, compute gating and calibration suppression passed\n";
}
