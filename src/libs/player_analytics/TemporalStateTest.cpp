#include "hstream/src/libs/player_analytics/TemporalState.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <string>

namespace pa = hm::player_analytics;

namespace {
bool Check(bool value, const char* message) {
  if (!value)
    std::cerr << message << '\n';
  return value;
}
pa::Pose Pose(float x = 250, float confidence = 0.9F) {
  pa::Pose pose{};
  for (auto& joint : pose)
    joint = {x, 500, confidence};
  return pose;
}
} // namespace

int main() {
  bool ok = true;
  pa::TrackScheduler scheduler(2, pa::kSecond);
  const uint64_t first[] = {100, 200, 300};
  scheduler.BeginFrame(1, 0, first, 3);
  const uint64_t incarnation = scheduler.Find(100)->incarnation;
  ok &= Check(
      scheduler.Find(100) && scheduler.Find(200) && !scheduler.Find(300) && scheduler.excluded_count() == 1,
      "capacity must exclude new identities without evicting visible state");
  std::array<uint64_t, pa::kMaximumDueRois> due{};
  ok &= Check(
      scheduler.SelectDue(pa::Feature::kPose, 100, &due, 1) == 1 && due[0] == 100,
      "oldest due order should be deterministic");
  scheduler.MarkAttempt(pa::Feature::kPose, 100);
  scheduler.BeginFrame(1, 100, first, 2);
  ok &= Check(
      scheduler.SelectDue(pa::Feature::kPose, 100, &due, 1) == 1 && due[0] == 200,
      "candidate cap must not starve a never-scheduled track");
  ok &= Check(!scheduler.BeginFrame(1, 100, first, 2), "duplicate frame must not rerun analytics");
  scheduler.BeginFrame(1, 2 * pa::kSecond, first, 1);
  ok &= Check(
      scheduler.Find(100)->incarnation != incarnation && !scheduler.Find(200),
      "expired/reused ID must receive fresh state identity");
  scheduler.BeginFrame(2, 0, first + 2, 1);
  ok &= Check(!scheduler.Find(100) && scheduler.Find(300), "new epoch must reset track state");
  ok &= Check(!scheduler.BeginFrame(2, pa::kInvalidTime, first, 1) && !scheduler.Find(300), "invalid PTS resets state");

  pa::JerseyConsensus jersey;
  ok &= Check(jersey.Observe(0, "07", 0.9F) && !jersey.Observe(0, "07", 0.9F), "duplicate OCR cannot double vote");
  ok &= Check(jersey.Result(0).text[0] == '\0', "one weak observation should not acquire a jersey");
  jersey.Observe(100000000, "07", 0.9F);
  ok &= Check(std::string(jersey.Result(100000000).text.data()) == "07", "leading zero was lost");
  jersey.Observe(200000000, "9", 0.9F);
  ok &= Check(std::string(jersey.Result(200000000).text.data()) == "07", "one conflicting vote should not flicker");
  ok &= Check(!jersey.Observe(300000000, "A7", 1) && !jersey.Observe(400000000, "777", 1), "invalid text accepted");
  ok &= Check(jersey.Result(4 * pa::kSecond).text[0] == '\0', "missing readings must expire held evidence");
  jersey.Observe(5 * pa::kSecond, "0", 0.9F);
  jersey.Observe(5 * pa::kSecond + 100000000, "0", 0.9F);
  ok &= Check(std::string(jersey.Result(5 * pa::kSecond + 100000000).text.data()) == "0", "jersey zero is not unknown");
  ok &= Check(jersey.Result(1).text[0] == '\0', "backward source time must clear jersey evidence");

  pa::ActionHistory history;
  auto pose = Pose();
  ok &= Check(history.Observe(0, pose, 1000, 1000), "initial pose rejected");
  for (uint64_t i = 1; i < 100; ++i)
    history.Observe(i * pa::kActionSamplePeriod, pose, 1000, 1000);
  ok &= Check(
      history.ready(9900000000ULL) && history.window_start() == 0 && history.window_end() == 9900000000ULL,
      "100 causal samples must span exactly 9.9 seconds");
  std::array<float, pa::ActionHistory::kTensorFloats> tensor{};
  ok &= Check(
      history.WriteTensor(9900000000ULL, tensor.data(), tensor.size()) && std::abs(tensor[0] + 0.5F) < 1e-6F &&
          tensor[1] == 0 && std::abs(tensor[2] - 0.9F) < 1e-6F,
      "action coordinates must normalize against the whole metadata canvas");
  ok &= Check(
      std::all_of(tensor.begin() + tensor.size() / 2, tensor.end(), [](float value) { return value == 0; }),
      "the second model person must remain zero padded");
  ok &= Check(
      !history.Observe(9900000000ULL, pose, 1000, 1000) && history.size() == 100,
      "duplicate pose must not add a temporal sample");
  ok &= Check(!history.ready(10050000001ULL), "stale skeleton history must become unready");
  history.Observe(10100000000ULL, pose, 1000, 1000);
  ok &= Check(history.size() == 1 && !history.ready(10100000000ULL), "over-150ms gap must restart history");

  history.Reset();
  history.Observe(0, Pose(0, 0.9F), 1000, 1000);
  history.Observe(120000000, Pose(120, 0.6F), 1000, 1000);
  ok &= Check(
      history.size() == 2 && history.window_end() == 100000000,
      "jittered observations should fill only bracketed uniform sample times");
  for (uint64_t i = 2; i < 100; ++i)
    history.Observe(i * pa::kActionSamplePeriod + 20000000, Pose(120, 0.6F), 1000, 1000);
  ok &= Check(
      history.WriteTensor(9920000000ULL, tensor.data(), tensor.size()) && std::abs(tensor[17 * 3] + 0.8F) < 1e-6F &&
          std::abs(tensor[17 * 3 + 2] - 0.6F) < 1e-6F,
      "interpolation must use source time and minimum endpoint confidence");
  pose[0].x = std::numeric_limits<float>::quiet_NaN();
  ok &= Check(
      !history.Observe(10020000000ULL, pose, 1000, 1000) && history.size() == 0,
      "nonfinite observations must clear history");
  ok &= Check(!history.Observe(pa::kInvalidTime, Pose(), 1000, 1000), "invalid timestamps cannot seed history");
  history.Observe(1, Pose(), 1000, 1000);
  history.Observe(100000001, Pose(), 2000, 1000);
  ok &= Check(history.size() == 1, "coordinate-space change must reset history");

  pa::ActionLabelState label;
  ok &= Check(label.Observe(10 * pa::kSecond, 0, 5, 0.8F), "first confident action should acquire");
  ok &= Check(!label.Observe(10 * pa::kSecond, 0, 6, 1), "duplicate action result must not vote twice");
  ok &= Check(
      !label.Observe(11 * pa::kSecond, pa::kSecond, 6, 0.81F) && label.Result(11 * pa::kSecond).label == 5,
      "single marginal challenger should not flicker action labels");
  ok &= Check(
      label.Observe(12 * pa::kSecond, 2 * pa::kSecond, 6, 0.81F), "two consistent action challengers should switch");
  ok &= Check(label.Result(15 * pa::kSecond).label == -1, "action labels cannot persist indefinitely");
  ok &= Check(!label.Observe(16 * pa::kSecond, 15 * pa::kSecond, 5, 1), "short action windows must be rejected");
  return ok ? 0 : 1;
}
