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
      !disabled->drawing.jersey) {
    std::cerr << "disabled parsing must ignore models/limits while preserving draw preferences\n";
    return 1;
  }
  for (const char* invalid : {
           "pose: {enable: 1}",
           "action: {enable: 1, bundle: x}",
           "jersey: {enable: 1, bundle: x, roi-mode: pose}",
           "pose: {enable: 1, bundle: x, rate-hz: .nan}",
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
  return 0;
}
