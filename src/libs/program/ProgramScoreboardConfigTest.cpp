#include "hstream/src/libs/program/ProgramScoreboardConfig.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
} // namespace
int main(int argc, char** argv) {
  try {
    Check(argc == 3, "Expected baseline and application YAML");
    const auto baseline = YAML::LoadFile(argv[1]), native = YAML::LoadFile(argv[2]);
    const auto game = YAML::Load(
        "rink: {stitched_output_generation: saved, scoreboard: {perspective_polygon: [[100, 100], [300, 100], [300, 200], [100, 200]]}}");
    const auto resolve = [&](const std::vector<std::string>& options,
                             const YAML::Node& selected = YAML::Node(YAML::NodeType::Map)) {
      const auto config =
          hm::config::ResolveProgramConfiguration(baseline, YAML::Node(YAML::NodeType::Map), selected, native, options);
      Check(config.ok(), config.status().ToString());
      return hm::program::ResolveProgramScoreboardConfig(*config, 3840, 2160);
    };
    auto result = resolve({}, game);
    Check(result.ok() && result->has_value(), result.status().ToString());
    Check(
        (*result)->requested_width == 384 && (*result)->requested_height == 432 &&
            (*result)->position == hm::scoreboard::Position::kUpperLeft,
        "Baseline dimensions/placement changed");
    auto plan = hm::program::PrepareProgramScoreboard(**result, "saved", 1000, 1000, 3840, 2160);
    Check(
        plan.ok() && plan->width == 384 && plan->height == 192 && plan->left == 0, "Native aspect adjustment changed");
    for (size_t i = 0; i < 4; ++i) {
      const double x = (i == 1 || i == 2) ? plan->width - 1 : 0;
      const double y = i >= 2 ? plan->height - 1 : 0;
      const auto* h = plan->inverse;
      const double denominator = h[6] * x + h[7] * y + h[8];
      Check(
          std::abs((h[0] * x + h[1] * y + h[2]) / denominator - (*result)->polygon[i][0]) < .001 &&
              std::abs((h[3] * x + h[4] * y + h[5]) / denominator - (*result)->polygon[i][1]) < .001,
          "Projected rectangle corner changed");
    }
    Check(
        !hm::program::PrepareProgramScoreboard(**result, "other", 1000, 1000, 3840, 2160).ok(),
        "Stale output generation accepted");
    Check(
        !hm::program::PrepareProgramScoreboard(**result, "saved", 200, 200, 3840, 2160).ok(),
        "Polygon outside panorama accepted");
    result = resolve({"pipeline.hmplaycropper.properties.scoreboard-position=upper-right"}, game);
    Check(result.ok() && result->has_value(), "Native placement rejected");
    plan = hm::program::PrepareProgramScoreboard(**result, "saved", 1000, 1000, 3840, 2160);
    Check(plan.ok() && plan->left == 3840 - plan->width, "Right placement changed");
    result = resolve(
        {"rink.scoreboard.position=upper-left,pipeline.hmplaycropper.private-properties.scoreboard-position=upper-center"},
        game);
    Check(
        result.ok() && result->has_value() && (*result)->position == hm::scoreboard::Position::kUpperCenter,
        "Native placement lost same-layer precedence");
    for (const auto* selection :
         {"pipeline.hmplaycropper.show-scoreboard=0", "pipeline.hmplaycropper.private-properties.show-scoreboard=0"}) {
      result = resolve({selection});
      Check(result.ok() && !result->has_value(), "Disabled scoreboard required polygon/resources");
    }
    auto disabled = YAML::Clone(game);
    disabled["rink"]["scoreboard"]["perspective_polygon"] = YAML::Load("[[0,0],[0,0],[0,0],[0,0]]");
    result = resolve({}, disabled);
    Check(result.ok() && !result->has_value(), "Disabled sentinel ignored");
    Check(!resolve({}).ok(), "Missing polygon accepted");
    for (const auto* setting :
         {"rink.scoreboard.scoreboard_scale=0",
          "rink.scoreboard.projected_width=%1000",
          "rink.scoreboard.projected_height=1e300",
          "rink.scoreboard.position=middle",
          "rink.scoreboard.resize_filter=nearest",
          "rink.scoreboard.sharpen_amount=1"})
      Check(!resolve({setting}, game).ok(), std::string("Unsupported scoreboard setting accepted: ") + setting);
    auto legacy = YAML::Clone(game);
    legacy["pipeline"]["hmplaycropper"]["scoreboard-perspective-polygon"] = "100,100,300,100,300,200,100,200";
    result = resolve({}, legacy);
    Check(result.ok() && result->has_value(), "Native polygon rejected");
    legacy["pipeline"]["hmplaycropper"]["scoreboard-perspective-polygon"] = "100,100,300,100,300,200,100,200,";
    Check(!resolve({}, legacy).ok(), "Trailing native polygon separator accepted");
    auto cleared = YAML::Clone(game);
    cleared["rink"]["scoreboard"]["position"] = YAML::Node(YAML::NodeType::Null);
    cleared["rink"]["scoreboard"]["projected_width"] = YAML::Node(YAML::NodeType::Null);
    result = resolve({}, cleared);
    Check(
        result.ok() && result->has_value() && (*result)->requested_width == 384 &&
            (*result)->position == hm::scoreboard::Position::kUpperLeft,
        "Null scalar settings did not restore baseline values");
    result = resolve({"pipeline.hmplaycropper.private-properties.scoreboard_projected_width=300"}, game);
    Check(result.ok() && result->has_value() && (*result)->requested_width == 300, "Native underscore alias ignored");
    auto bad = YAML::Clone(game);
    bad["rink"]["scoreboard"]["perspective_polygon"] = YAML::Load("[[100,100],[300,200],[300,100],[100,200]]");
    result = resolve({}, bad);
    Check(
        result.ok() && result->has_value() &&
            !hm::program::PrepareProgramScoreboard(**result, "saved", 1000, 1000, 3840, 2160).ok(),
        "Self-intersecting polygon accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
