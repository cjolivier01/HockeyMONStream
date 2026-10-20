#include "hstream/src/libs/player_analytics/Config.h"

#include <iostream>

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
  // arrives without them is a plumbing bug and must fail rather than invent a
  // default. Out-of-range values fail the same way.
  for (const char* invalid_team : {
           "team: {enable: 1}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 600, min-margin: 0.12}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 600}",
           "team: {enable: 1, warmup-samples: 200, min-margin: 0.12}",
           "team: {enable: 1, refit-interval: 600, min-margin: 0.12}",
           "team: {enable: 1, warmup-samples: 0, refit-interval: 600, min-margin: 0.12}",
           "team: {enable: 1, warmup-samples: 513, refit-interval: 600, min-margin: 0.12}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 0, min-margin: 0.12}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 600, min-margin: 1.0}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 600, min-margin: -0.1}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 600, min-margin: .nan}",
           "team: {enable: 1, warmup-samples: 200, refit-interval: 600, min-margin: 0.12, confidence-threshold: 0.5}",
       }) {
    if (pa::ParseConfig(YAML::Load(invalid_team)).ok()) {
      std::cerr << "team configuration without baseline clustering values accepted: " << invalid_team << '\n';
      return 6;
    }
  }
  const auto team =
      pa::ParseConfig(YAML::Load("team: {enable: 1, rate-hz: 2, warmup-samples: 200, refit-interval: 600, "
                                 "min-margin: 0.12}\n"
                                 "home-is-dark: true\ndraw-teams: 1\n"));
  if (!team.ok() || !team->team.enabled || !team->drawing.team ||
      team->team.model != "reidentificationnet-deployable-v1.2" || team->team.rate_hz != 2 ||
      team->team_clustering.warmup_samples != 200 ||
      team->team_clustering.refit_interval != 600 || team->team_clustering.minimum_margin != 0.12F ||
      team->home_is_dark != true || (team->drawing_layers() & pa::kDrawTeams) == 0) {
    std::cerr << "a complete team configuration must parse with its baseline tuning intact\n";
    return 7;
  }
  // Team needs no pose, so it must not drag the pose dependency in with it.
  const auto alone =
      pa::ParseConfig(YAML::Load("team: {enable: 1, rate-hz: 2, warmup-samples: 64, refit-interval: 256, min-margin: 0}\n"));
  if (!alone.ok() || !alone->enabled() || alone->pose.enabled || alone->home_is_dark.has_value()) {
    std::cerr << "team recognition must stand alone and leave the side unnamed when not told\n";
    return 8;
  }
  // A disabled team feature must not be held to the baseline requirement.
  const auto off = pa::ParseConfig(YAML::Load("team: {enable: 0}\npose: {enable: 1}\n"));
  if (!off.ok() || off->team.enabled) {
    std::cerr << "a disabled team feature must not require clustering values\n";
    return 9;
  }
  return 0;
}
