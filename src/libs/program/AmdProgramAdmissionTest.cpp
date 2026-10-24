#include "hstream/src/libs/program/AmdProgramAdmission.h"

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
    Check(argc == 3, "Expected baseline and native YAML");
    const auto baseline = YAML::LoadFile(argv[1]), native = YAML::LoadFile(argv[2]);
    const auto check = [&](const std::vector<std::string>& options) {
      auto config = hm::config::ResolveProgramConfiguration(
          baseline, YAML::Node(YAML::NodeType::Map), YAML::Node(YAML::NodeType::Map), native, options);
      Check(config.ok(), config.status().ToString());
      return hm::program::ValidateAmdProgramAdmission(*config);
    };
    Check(check({}).ok(), "Default Program stages were rejected");
    const std::string video = "pipeline.hmaudio.enable=0,pipeline.hmplaycropper.show-scoreboard=0";
    Check(check({video}).ok(), check({video}).ToString());
    for (const auto* unsupported :
         {"pipeline.tracker.enable=0",
          "pipeline.player-analytics.pose.enable=true",
          "plot.plot_individual_player_tracking=true",
          "pipeline.primary-gie.interval=1",
          "pipeline.ds-playtracker.private-properties.telemetry-db-dir=/tmp/log",
          "stitching.enabled=false",
          "pipeline.hmstitcher.enable=0",
          "pipeline.hmstitcher.configure-only=1",
          "stitching.post_stitch_rotate_degrees=90",
          "game.stitching.stitch_rotate_degrees=90",
          "pipeline.hmstitcher.post_stitch_rotate_degrees=90",
          "pipeline.hmstitcher.post-stitch-rotate-degrees=90",
          "pipeline.hmstitcher.private-properties.post-stitch-rotate-degrees=90",
          "pipeline.ds-example.enable=1",
          "pipeline.dewarper0.enable=1",
          "pipeline.hm-image-meta-merger.enable=1",
          "pipeline.tracker.reid-enable=1",
          "team.enabled=true",
          "pipeline.hmplaycropper.plot-play-tracking=1",
          "pipeline.ds-playtracker.draw=1",
          "pipeline.hmplaycropper.private-properties.plot-play-tracking=1",
          "pipeline.secondary-gie0.enable=1",
          "pipeline.hmplaycropper.properties.plot-player-tracking=1",
          "pipeline.ds-playtracker.private-properties.rink-camera-enable=true"})
      Check(!check({video, unsupported}).ok(), std::string("Unsupported request accepted: ") + unsupported);
    Check(
        absl::IsInvalidArgument(check({video, "pipeline.player-analytics.pose.enable=maybe"})),
        "Malformed flag accepted");
    Check(
        check({video, "team.enabled=true,pipeline.player-analytics.team.enable=false"}).ok(),
        "Native disabled team choice lost precedence");
    Check(
        check({video, "plot.plot_individual_player_tracking=true,pipeline.hmplaycropper.plot-player-tracking=0"}).ok(),
        "Native disabled drawing choice lost precedence");
    Check(
        check({video, "plot.plot_pose=true,pipeline.player-analytics.draw-pose=0"}).ok(),
        "Native disabled pose drawing choice lost precedence");
    Check(
        check({video, "stitching.enabled=false,pipeline.hmstitcher.enable=1"}).ok(),
        "Native enabled stitching lost precedence");
    for (const auto* lower :
         {"pipeline: {hmstitcher: {post-stitch-rotate-degrees: 90}}", "stitching: {stitch_rotate_degrees: 90}"}) {
      auto config = hm::config::ResolveProgramConfiguration(
          baseline,
          YAML::Load(lower),
          YAML::Node(YAML::NodeType::Map),
          native,
          {video, "stitching.post_stitch_rotate_degrees=null"});
      Check(
          config.ok() && hm::program::ValidateAmdProgramAdmission(*config).ok(),
          "Explicit null did not clear lower-layer rotation");
      config = hm::config::ResolveProgramConfiguration(
          baseline, YAML::Load(lower), YAML::Load("stitching: {post_stitch_rotate_degrees: null}"), native, {video});
      Check(
          config.ok() && hm::program::ValidateAmdProgramAdmission(*config).ok(),
          "Game null did not clear user rotation");
    }
    Check(
        !check({video, "stitching.post_stitch_rotate_degrees=null,pipeline.hmstitcher.post-stitch-rotate-degrees=90"})
             .ok(),
        "Canonical null cleared same-layer native rotation");
    Check(
        absl::IsInvalidArgument(check({video, "stitching.post_stitch_rotate_degrees=.nan"})),
        "Nonfinite rotation accepted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
