#include "hstream/src/libs/player_analytics/Config.h"

#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pa = hm::player_analytics;

int main() {
  auto absent = pa::ParseConfig(YAML::Node());
  auto disabled = pa::ParseConfig(
      YAML::Load(
          "pose: {enable: 0, bundle: [invalid, nonexistent], rate-hz: nope}\n"
          "jersey: {enable: false, roi-mode: invalid}\n"
          "action: {enable: 0, bundle: /no/such/model}\n"
          "max-tracks: invalid\ndraw-pose: true\ndraw-jerseys: 1\n"));
  if (!absent.ok() || absent->enabled() || !disabled.ok() || disabled->enabled() || !disabled->drawing.pose ||
      !disabled->drawing.jersey || !absent->continuous_pose_drawing || absent->pose.rate_hz != 10 ||
      absent->pose.every_frame) {
    std::cerr << "disabled parsing must ignore models/limits while preserving draw preferences\n";
    return 1;
  }
  for (const char* invalid : {
           "pose: {enable: 1, model: custom}",
           "pose: {enable: 1, model: unknown}",
           "pose: {enable: 1, model: parseq-hockey-cvprw2024}",
           "pose: {enable: 1, bundle: x}\njersey: {enable: 1, bundle: y, roi-mode: pose}\nmax-due-rois: 1",
           "action: {enable: 1, bundle: x}",
           "jersey: {enable: 1, bundle: x, roi-mode: pose}",
           "pose: {enable: 1, bundle: x, rate-hz: .nan}",
           "pose: {enable: 1, rate-hz: 0}",
           "pose: {enable: 1, rate-hz: [fps]}",
           "pose: {enable: 1, rate-hz: unknown}",
           "jersey: {enable: 1, rate-hz: fps}",
           "pose: {enable: 1}\naction: {enable: 1, rate-hz: fps}",
           "pose: {continuous-drawing: invalid}",
           "pose: {enable: 1, bundle: x, rate-hz: 9}\naction: {enable: 1, bundle: y}",
           "pose: {enable: 1, bundle: x}\nmax-tracks: 257",
           "pose: {enable: 1, bundle: x}\nmax-due-rois: 33",
           "pose: {enable: 1, bundle: x}\nbatch-size: 9",
           "pose: {enable: 1, bundle: x}\nmax-due-rois: 2\nbatch-size: 3",
       }) {
    if (pa::ParseConfig(YAML::Load(invalid)).ok()) {
      std::cerr << "invalid enabled config accepted: " << invalid << '\n';
      return 2;
    }
  }
  auto valid = pa::ParseConfig(
      YAML::Load(
          "jersey: {enable: 1, bundle: /deliberately/nonexistent, roi-mode: bbox}\n"
          "pose: {enable: 0, bundle: [invalid]}\nmax-due-rois: 3\n"));
  if (!valid.ok() || !valid->jersey.enabled || valid->pose.enabled || valid->batch_size != 3) {
    std::cerr << "pure parsing must allow bbox-only jersey with a path whose existence is checked later\n";
    return 3;
  }
  for (const char* builtin :
       {"pose: {enable: 1}",
        "pose: {enable: 1, model: null}",
        "pose: {enable: 1, model: rtmpose-m-coco17-256x192, bundle: [old, invalid]}"}) {
    const auto selected = pa::ParseConfig(YAML::Load(builtin));
    if (!selected.ok() || selected->pose.model != "rtmpose-m-coco17-256x192" || !selected->pose.bundle.empty()) {
      std::cerr << "built-in model selection must not require a prepared path\n";
      return 4;
    }
  }
  if (valid->jersey.model != "custom") {
    std::cerr << "legacy explicit paths must retain their custom-model semantics\n";
    return 5;
  }
  const auto fps = pa::ParseConfig(
      YAML::Load("pose: {enable: true, rate-hz: fps, continuous-drawing: false}\naction: {enable: true}"));
  if (!fps.ok() || !fps->pose.every_frame || fps->continuous_pose_drawing)
    return 7;
  const auto numeric = pa::ParseConfig(YAML::Load("pose: {enable: true, rate-hz: 29.97}"));
  if (!numeric.ok() || numeric->pose.every_frame || numeric->pose.rate_hz != 29.97)
    return 8;
  // The baseline owns every clustering value, so an enabled team feature that
  // arrives without one is a plumbing bug and must fail rather than invent a
  // default. Out-of-range values fail the same way.
  const auto team_yaml = [](const std::map<std::string, std::string>& overrides) {
    std::map<std::string, std::string> keys{
        {"rate-hz", "2"},
        {"warmup-samples", "200"},
        {"refit-interval", "600"},
        {"min-margin", "0.12"},
        {"min-lightness-gap", "6.0"},
        {"min-cluster-share", "0.25"},
        {"acquisition-evidence", "1.0"},
        {"switch-hysteresis", "1.35"},
        {"min-track-height", "60"}};
    for (const auto& [key, value] : overrides) {
      if (value.empty())
        keys.erase(key);
      else
        keys[key] = value;
    }
    std::string text = "team: {enable: 1";
    for (const auto& [key, value] : keys)
      text += ", " + key + ": " + value;
    return text + "}\n";
  };
  // Every required key, dropped one at a time, then each range violated.
  std::vector<std::string> invalid_team{"team: {enable: 1}\n"};
  for (const char* key : {"rate-hz", "warmup-samples", "refit-interval", "min-margin", "min-lightness-gap",
                          "min-cluster-share", "acquisition-evidence", "switch-hysteresis", "min-track-height"})
    invalid_team.push_back(team_yaml({{key, ""}}));
  for (const auto& bad : std::vector<std::pair<const char*, const char*>>{
           {"warmup-samples", "0"}, {"warmup-samples", "513"}, {"refit-interval", "0"}, {"min-margin", "1.0"},
           {"min-margin", "-0.1"}, {"min-margin", ".nan"}, {"min-lightness-gap", "101"},
           {"min-lightness-gap", "-1"}, {"min-cluster-share", "0.6"}, {"min-cluster-share", "-0.1"},
           {"acquisition-evidence", "0"}, {"switch-hysteresis", "0.9"}, {"min-track-height", "-1"},
           {"confidence-threshold", "0.5"}})
    invalid_team.push_back(team_yaml({{bad.first, bad.second}}));
  for (const auto& text : invalid_team) {
    if (pa::ParseConfig(YAML::Load(text)).ok()) {
      std::cerr << "invalid team configuration accepted: " << text;
      return 6;
    }
  }
  const auto team = pa::ParseConfig(YAML::Load(team_yaml({}) + "home-is-dark: true\ndraw-teams: 1\n"));
  if (!team.ok() || !team->team.enabled || !team->drawing.team ||
      team->team.model != "reidentificationnet-deployable-v1.2" || team->team.rate_hz != 2 ||
      team->team_clustering.warmup_samples != 200 || team->team_clustering.refit_interval != 600 ||
      team->team_clustering.minimum_margin != 0.12F || team->team_clustering.minimum_lightness_gap != 6.0F ||
      team->team_clustering.minimum_cluster_share != 0.25F || team->team_clustering.acquisition_evidence != 1.0 ||
      team->team_clustering.switch_hysteresis != 1.35 || team->team_clustering.minimum_track_height != 60.0F ||
      team->home_is_dark != true || (team->drawing_layers() & pa::kDrawTeams) == 0) {
    std::cerr << "a complete team configuration must parse with its baseline tuning intact\n";
    return 7;
  }
  // The configurator writes mapped flags as 1/0, which yaml-cpp's bool
  // conversion rejects; home-is-dark must read both spellings.
  for (const char* spelling : {"1", "true", "yes", "on"}) {
    const auto flagged = pa::ParseConfig(YAML::Load(team_yaml({}) + "home-is-dark: " + spelling + "\n"));
    if (!flagged.ok() || flagged->home_is_dark != true) {
      std::cerr << "home-is-dark spelling not understood: " << spelling << '\n';
      return 8;
    }
  }
  // Team needs no pose, so it must not drag the pose dependency in with it,
  // and an unset home_is_dark must stay unset rather than defaulting a side.
  const auto alone = pa::ParseConfig(YAML::Load(team_yaml({})));
  if (!alone.ok() || !alone->enabled() || alone->pose.enabled || alone->home_is_dark.has_value()) {
    std::cerr << "team recognition must stand alone and leave the side unnamed when not told\n";
    return 9;
  }
  // A disabled team feature must not be held to the baseline requirement.
  const auto off = pa::ParseConfig(YAML::Load("team: {enable: 0}\npose: {enable: 1}\n"));
  if (!off.ok() || off->team.enabled) {
    std::cerr << "a disabled team feature must not require clustering values\n";
    return 10;
  }
  return 0;
}
