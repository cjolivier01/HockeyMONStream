#include "hstream/src/apps/apps-common/PlayerAnalyticsBin.h"

#include <cstdlib>
#include <iostream>

namespace {
void Expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::exit(1);
  }
}
} // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  using hm::gst::ResolvePlayerAnalyticsConfig;
  for (const char* text :
       {"{}",
        "player-analytics: {pose: {enable: 0, bundle: /proc/1/mem}, gpu-id: broken, batch-size: broken}",
        "player-analytics: {draw-pose: 1, draw-jerseys: 1, draw-actions: 1}"}) {
    const auto config = ResolvePlayerAnalyticsConfig(YAML::Load(text), "/nonexistent", -99);
    Expect(config.ok(), "disabled analytics must ignore inference paths and limits");
    Expect(!config->analytics.enabled() && config->serialized.empty(), "disabled analytics must remain empty");
    const auto element = hm::gst::CreatePlayerAnalytics(*config, false);
    Expect(element.ok() && !*element, "disabled analytics must omit factory creation even without tracker/plugin");
  }
  const auto enabled = ResolvePlayerAnalyticsConfig(
      YAML::Load("player-analytics: {pose: {enable: 1, bundle: models/pose}, gpu-id: 2}"), "/config", 5);
  Expect(enabled.ok() && enabled->analytics.pose.enabled && enabled->gpu_id == 2, "explicit GPU wins");
  const auto node = YAML::Load(enabled->serialized);
  Expect(node["pose"]["bundle"].as<std::string>() == "/config/models/pose", "relative bundle resolves lexically");
  Expect(!hm::gst::CreatePlayerAnalytics(*enabled, false).ok(), "enabled analytics requires native tracker");
  const auto inherited = ResolvePlayerAnalyticsConfig(
      YAML::Load("player-analytics: {pose: {enable: 1, bundle: /models/pose}}"), "/config", 3);
  Expect(inherited.ok() && inherited->gpu_id == 3, "application GPU is inherited");
  Expect(
      YAML::Load(inherited->serialized)["pose"]["bundle"].as<std::string>() == "/models/pose",
      "absolute bundle remains unchanged");
  for (const char* text :
       {"player-analytics: {pose: {enable: 1}}",
        "player-analytics: {action: {enable: 1, bundle: /action}}",
        "player-analytics: {pose: {enable: 1, bundle: /pose}, gpu-id: -1}"})
    Expect(!ResolvePlayerAnalyticsConfig(YAML::Load(text), "/config", 0).ok(), "invalid enabled config rejects");
  std::cout << "player analytics app configuration passed\n";
}
