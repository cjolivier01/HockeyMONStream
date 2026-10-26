#include "hstream/src/libs/tracking/PlayTrackerRuntimeControls.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}
} // namespace

int main() {
  const auto baseline = YAML::LoadFile("configs/baseline.yaml");
  const auto resolved_defaults = hm::tracking::ReadPlaytrackerControlValues(baseline);
  Check(resolved_defaults.ok(), "Required baseline camera controls were rejected");
  const auto defaults = *resolved_defaults;
  auto values = defaults;
  const auto source = YAML::Load(R"(
play-tracker:
  camera-name: private-native-camera
  untouched: custom
  hstream-runtime-tuning: {max-speed-y: 99}
  live-boxes:
    - {name: current_roi_aspect, max-speed-x: 7, max-speed-y: 8}
    - {name: current_roi, max-speed-x: 9, max-speed-y: 10}
)");
  values["Apply_To_Fast_Box"] = 1;
  values["Apply_To_Follower_Box"] = 0;
  values["Max_Speed_X_x10"] = 123;
  values["Overshoot_Speed_Ratio_x100"] = 0;
  values["Ignore_Oversized_Players"] = 1;
  values["Oversized_Player_Percent"] = 125.25;
  values["Stop_Direction_Change_Delay_Frames"] = 17;
  const auto result = hm::tracking::BuildPlaytrackerRuntimeControls(
      source,
      values,
      defaults,
      {"Max_Speed_X_x10",
       "Overshoot_Speed_Ratio_x100",
       "Ignore_Oversized_Players",
       "Oversized_Player_Percent",
       "Stop_Direction_Change_Delay_Frames"},
      false);
  Check(result.ok(), "Valid immutable runtime delta was rejected");
  const auto tracker = (*result)["play-tracker"];
  const auto tuning = tracker["hstream-runtime-tuning"];
  Check(tuning.size() == 5 && !tuning["max-speed-y"].IsDefined(), "A stale delta entered the new snapshot");
  Check(
      tuning["max-speed-x"].as<double>() == 12.3 && tuning["overshoot-scale-speed-ratio"].as<double>() == 0.01,
      "Desktop step conversion or minimum overshoot ratio changed");
  Check(tuning["oversized-bbox-percent"].as<double>() == 125.25, "A desktop percentage lost its fractional step");
  Check(
      tuning["ignore-oversized-bboxes"].as<bool>() && tuning["stop-translation-on-dir-change-delay"].as<int>() == 17,
      "Boolean or integer runtime fields changed units");
  Check(
      tracker["live-boxes"][0]["max-speed-x"].as<double>() == 7 &&
          tracker["live-boxes"][1]["max-speed-x"].as<double>() == 12.3 &&
          tracker["live-boxes"][1]["max-speed-y"].as<double>() == 10,
      "Native fast/follower role order or untouched base values changed");
  Check(
      tracker["camera-name"].as<std::string>() == "private-native-camera" &&
          tracker["untouched"].as<std::string>() == "custom" &&
          source["play-tracker"]["live-boxes"][1]["max-speed-x"].as<int>() == 9,
      "Publication mutated its base document or discarded configured fields");
  const auto both = hm::tracking::BuildPlaytrackerRuntimeControls(source, values, defaults, {"Max_Speed_X_x10"}, true);
  Check(
      both.ok() && (*both)["play-tracker"]["hstream-apply-to-fast-box"].as<bool>() &&
          (*both)["play-tracker"]["hstream-apply-to-follower-box"].as<bool>(),
      "Forced replay did not target both configured roles");
  auto invalid = values;
  invalid["Max_Speed_X_x10"] = 2001;
  Check(
      !hm::tracking::BuildPlaytrackerRuntimeControls(source, invalid, defaults, {"Max_Speed_X_x10"}, false).ok(),
      "Out-of-range runtime speed was admitted");
  auto incomplete = defaults;
  incomplete.erase("Max_Speed_X_x10");
  Check(
      !hm::tracking::BuildPlaytrackerRuntimeControls(source, values, incomplete, {}, false).ok(),
      "Missing baseline control acquired an independent default");
  Check(
      !hm::tracking::BuildPlaytrackerRuntimeControls(source, values, defaults, {"unknown"}, false).ok(),
      "Unknown control entered a native runtime document");
  const auto empty = hm::tracking::BuildPlaytrackerRuntimeControls(source, defaults, defaults, {}, false);
  Check(
      empty.ok() && (*empty)["play-tracker"]["hstream-runtime-tuning"].size() == 0,
      "An unchanged control created a sparse runtime override");
  return 0;
}
