#include "hstream/src/libs/tracking/NativeTracker.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace hm::tracking;
using hm::detection::Proposal;
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
TrackerConfig Config() {
  return {.1f, .6f, .7f, .8f, .5f, .7f, true, 2, 2, 16, 1024 * 1024};
}
Proposal Box(float x, float confidence, int label = 0) {
  return {x, 20, x + 20, 60, confidence, label, 0};
}
std::unique_ptr<NativeTracker> Tracker(const TrackerConfig& config) {
  auto result = NativeTracker::Create(config);
  Check(result.ok(), "create tracker");
  return std::move(*result);
}
std::vector<Observation> Step(NativeTracker& tracker, uint64_t sequence, const std::vector<Proposal>& boxes) {
  auto result = tracker.Update(sequence, boxes);
  if (!result.ok())
    throw std::runtime_error(result.status().ToString());
  return *result;
}
void Lifecycle() {
  auto tracker = Tracker(Config());
  Check(Step(*tracker, 0, {Box(10, .9f)}).empty(), "new tracks require confirmation");
  auto frame = Step(*tracker, 1, {Box(11, .9f)});
  Check(frame.size() == 1, "confirmed track");
  const auto id = frame.front().id;
  frame = Step(*tracker, 2, {Box(12, .3f)});
  Check(frame.size() == 1 && frame.front().id == id && frame.front().box.confidence == .3f, "low-score continuation");
  Check(Step(*tracker, 3, {}).empty(), "lost prediction is not a fresh observation");
  Check(Step(*tracker, 4, {Box(14, .3f)}).empty(), "low score cannot recover lost track");
  frame = Step(*tracker, 5, {Box(15, .9f)});
  Check(frame.size() == 1 && frame.front().id == id, "recover after two missed frames");
  Step(*tracker, 6, {});
  Step(*tracker, 7, {});
  Step(*tracker, 8, {});
  Check(Step(*tracker, 9, {Box(19, .9f)}).empty(), "expired identity starts tentative");
  frame = Step(*tracker, 10, {Box(20, .9f)});
  Check(frame.size() == 1 && frame.front().id != id, "expired identity is not reused");
  const auto second_id = frame.front().id;
  Check(!tracker->Update(12, {}).ok(), "gaps require explicit reset");
  Check(Step(*tracker, 11, {Box(21, .9f)}).front().id == second_id, "rejected gap preserves state");
  tracker->Reset();
  Step(*tracker, 100, {Box(10, .9f)});
  frame = Step(*tracker, 101, {Box(11, .9f)});
  Check(frame.size() == 1 && frame.front().id > second_id, "reset cannot reuse an earlier track ID");
}
void Association() {
  auto config = Config();
  config.confirmation_frames = 1;
  auto tracker = Tracker(config);
  auto frame = Step(*tracker, 0, {Box(10, .9f), Box(40, .9f)});
  Check(frame.size() == 2, "two tracks");
  const auto left_id = frame[0].id, right_id = frame[1].id;
  frame = Step(*tracker, 1, {Box(38, .9f), Box(12, .9f)});
  Check(
      frame.size() == 2 && frame[0].id == left_id && frame[0].box.left == 12 && frame[1].id == right_id &&
          frame[1].box.left == 38,
      "association is independent of detection order");
  frame = Step(*tracker, 2, {Box(14, .9f, 1), Box(36, .9f)});
  Check(frame.size() == 2 && frame[0].id == right_id && frame[1].id != left_id, "classes never share an identity");
  const auto class_id = frame[1].id;
  frame = Step(*tracker, 3, {Box(16, .9f, 1), Box(34, .9f), Box(14, .9f)});
  Check(
      frame.size() == 3 && frame[0].id == left_id && frame[1].id == right_id && frame[2].id == class_id,
      "global matching recovers correct class and track");
}
void GlobalAssignment() {
  auto config = Config();
  config.confirmation_frames = 1;
  config.weight_high_matches_by_score = false;
  config.high_match_cost = .9f;
  auto tracker = Tracker(config);
  const auto initial = Step(*tracker, 0, {Box(10, .9f), Box(25, .9f)});
  const auto next = Step(*tracker, 1, {Box(16, .9f), Box(6, .9f)});
  Check(
      next.size() == 2 && next[0].id == initial[0].id && next[0].box.left == 6 && next[1].id == initial[1].id &&
          next[1].box.left == 16,
      "global assignment must resolve an ambiguous first detection using both tracks");
}
void InvalidAndBounded() {
  auto config = Config();
  config.low_score = .8f;
  Check(!NativeTracker::Create(config).ok(), "invalid score ordering");
  config = Config();
  config.maximum_tracks = 1;
  config.confirmation_frames = 1;
  auto tracker = Tracker(config);
  Check(!tracker->Update(0, {Box(1, .9f), Box(20, .9f)}).ok(), "reject excessive observations before mutation");
  Proposal bad = Box(1, .9f);
  bad.left = std::numeric_limits<float>::quiet_NaN();
  Check(!tracker->Update(0, {bad}).ok(), "reject nonfinite coordinates before prediction");
  auto first = Step(*tracker, 0, {Box(1, .9f)});
  Check(first.size() == 1 && first[0].id == 1, "invalid inputs leave identity allocation unchanged");
  Check(!tracker->Update(1, {Box(100, .9f)}).ok(), "live/lost track storage is bounded");
  Check(!tracker->Update(1, {}).ok(), "partial processing failure requires reset");
  tracker->Reset();
  Check(Step(*tracker, 0, {Box(100, .9f)}).size() == 1, "reset recovers retired tracker");
  config = Config();
  config.confirmation_frames = 1;
  config.maximum_cost_matrix_bytes = sizeof(double);
  tracker = Tracker(config);
  Step(*tracker, 0, {Box(1, .9f), Box(30, .9f)});
  Check(!tracker->Update(1, {Box(2, .9f), Box(31, .9f)}).ok(), "bounded assignment matrix");
}
} // namespace
int main() {
  try {
    Lifecycle();
    Association();
    GlobalAssignment();
    InvalidAndBounded();
    std::cout << "Native tracker association, lifecycle and bounds passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
