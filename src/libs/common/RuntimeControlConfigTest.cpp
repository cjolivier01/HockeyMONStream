#include "hstream/src/libs/common/RuntimeControlConfig.h"

#include <cmath>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigLayers.h"
#include "hstream/src/libs/common/ConfigPath.h"

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
    const auto user = YAML::Load(
        "rink: {camera: {motion_overrides: {max_speed_x: 7}}}\n"
        "video_out: {tone: {exposure: 0.4}}\nplayback: {start_seconds: 12}");
    const auto game = YAML::Load(
        "hstream_ui: {camera_controls: {Max_Speed_X_x10: 123, Exposure_x100: 73, "
        "Apply_To_Fast_Box: 1}, playback_start_time: '01:02:03.456'}");
    const auto before = YAML::Dump(game);
    auto resolved = hm::config::MergeNodes(YAML::Clone(user), game, false);
    Check(
        hm::get_node(resolved, "rink.camera.motion_overrides.max_speed_x")->as<double>() == 12.3,
        "Legacy game setting did not override canonical user setting");
    Check(
        hm::get_node(resolved, "video_out.tone.exposure")->as<double>() == .73,
        "Legacy exposure was not converted to native units");
    Check(
        hm::get_node(resolved, "playback.start_seconds")->as<double>() == 3723.456,
        "Legacy playback time lost millisecond precision");
    Check(
        hm::config::RuntimeControlPresentationValue(resolved, "Apply_To_Fast_Box") == 1,
        "Legacy target did not become a runtime target");
    Check(
        YAML::Dump(game) == before && !hm::get_node(resolved, "hstream_ui.camera_controls"),
        "Normalization changed the saved input or retained duplicate defaults");
    auto mutable_config = YAML::Load("pipeline: {sink0: {output-file: before.mkv}}");
    auto retained = mutable_config["pipeline"]["sink0"];
    hm::config::MergeNodes(mutable_config, YAML::Load("pipeline: {sink0: {output-file: after.mkv}}"), false);
    Check(retained["output-file"].as<std::string>() == "after.mkv", "Merge detached existing runtime YAML handles");
    hm::config::LeafRanks ranks;
    hm::config::RecordLeafRanks(game, {}, 2, ranks);
    Check(
        ranks.at("rink.camera.motion_overrides.max_speed_x") == 2 && ranks.at("video_out.tone.exposure") == 2,
        "Migrated keys lost their layer precedence");
    auto tie = YAML::Clone(game);
    tie["video_out"]["tone"]["exposure"] = .2;
    tie["rink"]["camera"]["motion_overrides"]["max_speed_x"] = 0;
    resolved = hm::config::MergeNodes(user, tie, false);
    Check(
        hm::get_node(resolved, "video_out.tone.exposure")->as<double>() == .2 &&
            hm::get_node(resolved, "rink.camera.motion_overrides.max_speed_x")->as<double>() == 0,
        "Canonical same-layer choice or explicit zero was masked by a legacy alias");
    const auto amd = YAML::Load("runtime: {amd: {output: {exposure: 0.6, realtime: true}}}");
    resolved = hm::config::MergeNodes(user, amd, false);
    Check(
        hm::get_node(resolved, "video_out.tone.exposure")->as<double>() == .6 &&
            hm::get_node(resolved, "playback.realtime")->as<bool>(),
        "Legacy AMD settings were lost");
    for (const auto* invalid : {"'00:60:00'", "'00:00:-1'", "'.nan'", "'1x'"}) {
      auto document = YAML::Load(std::string("hstream_ui: {playback_start_time: ") + invalid + "}");
      bool rejected = false;
      try {
        hm::config::NormalizeRuntimeControlLayer(document);
      } catch (const std::exception&) {
        rejected = true;
      }
      Check(rejected, "Malformed legacy playback time accepted");
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
