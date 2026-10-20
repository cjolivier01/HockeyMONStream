#include "hstream/src/apps/apps-common/PlayerAnalyticsBin.h"
#include "hstream/src/libs/player_analytics/Config.h"
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
        {"plot_actions", "draw-actions"},
        {"plot_teams", "draw-teams"}}) {
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
  for (const char* key : {"plot_pose", "plot_jersey_numbers", "plot_actions", "plot_teams"})
    document["plot"][key] = true;
  hm::Configurator configurator("unused", "/not-opened", -1);
  Map(configurator, document);
  auto resolved = hm::gst::ResolvePlayerAnalyticsConfig(configurator.config()["pipeline"], "/not-opened", -1);
  Require(
      resolved.ok() && !resolved->analytics.enabled() && resolved->serialized.empty() &&
          resolved->analytics.drawing.pose && resolved->analytics.drawing.jersey &&
          resolved->analytics.drawing.action && resolved->analytics.drawing.team &&
          resolved->analytics.drawing_layers() == 0,
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

void PoseCadenceAndDrawing(const YAML::Node& baseline) {
  hm::Configurator configurator("unused", "/not-opened", -1);
  Map(configurator, baseline);
  const std::string prefix = "pipeline.player-analytics.pose.";
  for (const auto& [key, value] :
       {std::pair{"enable", "true"},
        {"bundle", "/not-opened/pose"},
        {"rate-hz", "fps"},
        {"continuous-drawing", "false"}})
    Require(configurator.apply_config_item(prefix + key, value).ok(), "Pose CLI override failed");
  const auto resolved = hm::gst::ResolvePlayerAnalyticsConfig(configurator.config()["pipeline"], "/not-opened", 0);
  Require(
      resolved.ok() && resolved->analytics.pose.every_frame && !resolved->analytics.continuous_pose_drawing,
      "CLI pose overrides did not replace baseline defaults");
  const auto plugin = hm::player_analytics::ParseConfig(YAML::Load(resolved->serialized));
  Require(
      plugin.ok() && plugin->pose.every_frame && !plugin->continuous_pose_drawing,
      "Serialized plugin settings lost fps or continuous drawing");
}
} // namespace

// The team feature reads tuning the baseline owns and a side name the game
// owns, none of which it can invent. Both are mapped rather than duplicated, so
// a missing mapping shows up only when the feature is actually switched on --
// which is what this covers.
void TeamSettingsReachTheFeature(const YAML::Node& baseline) {
  auto document = YAML::Clone(baseline);
  document["team"]["enabled"] = true;
  document["game"]["home_is_dark"] = true;
  document["pipeline"] = YAML::Node(YAML::NodeType::Map);
  hm::Configurator configurator("unused", "/not-opened", -1);
  Map(configurator, document);
  const auto analytics = configurator.config()["pipeline"]["player-analytics"];
  const auto parsed = hm::player_analytics::ParseConfig(analytics);
  Require(parsed.ok(), "Baseline team settings did not survive mapping: " + std::string(parsed.status().message()));
  Require(parsed->team.enabled, "team.enabled did not reach the native feature");
  Require(
      parsed->team_clustering.warmup_samples == baseline["team"]["warmup_samples"].as<size_t>() &&
          parsed->team_clustering.refit_interval == baseline["team"]["refit_interval"].as<size_t>() &&
          parsed->team_clustering.minimum_margin == baseline["team"]["min_margin"].as<float>() &&
          parsed->team.rate_hz == baseline["team"]["rate_hz"].as<double>(),
      "Clustering tuning did not come from the baseline");
  Require(
      parsed->team_clustering.minimum_lightness_gap == baseline["team"]["min_lightness_gap"].as<float>() &&
          parsed->team_clustering.minimum_cluster_share == baseline["team"]["min_cluster_share"].as<float>() &&
          parsed->team_clustering.acquisition_evidence == baseline["team"]["acquisition_evidence"].as<double>() &&
          parsed->team_clustering.switch_hysteresis == baseline["team"]["switch_hysteresis"].as<double>() &&
          parsed->team_clustering.minimum_track_height == baseline["team"]["min_track_height"].as<float>(),
      "Ranking, balance, commitment and size thresholds did not come from the baseline");
  Require(
      parsed->team.model == baseline["team"]["model"].as<std::string>(), "team.model did not come from the baseline");
  Require(
      parsed->home_is_dark.has_value() && *parsed->home_is_dark,
      "game.home_is_dark never reached the feature, so the badge can only read DARK/LIGHT");

  // Null is the honest "nobody said", and must stay unnamed rather than guess.
  auto unnamed = YAML::Clone(document);
  unnamed["game"]["home_is_dark"] = YAML::Node();
  hm::Configurator second("unused", "/not-opened", -1);
  Map(second, unnamed);
  const auto without = hm::player_analytics::ParseConfig(second.config()["pipeline"]["player-analytics"]);
  Require(without.ok() && !without->home_is_dark.has_value(), "An unset home_is_dark was resolved to a side");
}

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  Require(argc == 2, "Usage: player_analytics_mapping_test BASELINE_YAML");
  const auto baseline = YAML::LoadFile(argv[1]);
  Precedence(baseline);
  DrawingAndCalibration(baseline);
  PoseCadenceAndDrawing(baseline);
  TeamSettingsReachTheFeature(baseline);
  std::cout << "Actual Configurator drawing precedence, compute gating and calibration suppression passed\n";
}
