#include "hstream/src/libs/stitching/RinkMap.h"

#include <cmath>
#include <iostream>

namespace {
namespace rink = hm::stitching;
const cv::Matx33d kCamera(1, 0, -3, 0, 1, 19, 0, 0, -7);
cv::Vec3d ray(cv::Point2d point) {
  const auto p = kCamera * cv::Vec3d(point.x, point.y, 1);
  return p / cv::norm(p);
}
rink::RinkMapObservation point(const rink::RinkLayout& layout, const std::string& id) {
  return {id, id, 0, false, false, true, 1, {{100, 100}}, {ray(*rink::RinkFeaturePoint(layout, id))}};
}
rink::RinkMapObservation line(const rink::RinkLayout& layout, const std::string& id) {
  const auto l = *rink::RinkFeatureLine(layout, id);
  const auto a = l[0] == 1 ? cv::Point2d(-l[2], -8) : cv::Point2d(-20, -l[2]);
  const auto b = l[0] == 1 ? cv::Point2d(-l[2], 8) : cv::Point2d(20, -l[2]);
  return {id, id, 0, true, true, true, 0.8, {{100, 100}, {200, 200}}, {ray(a), ray(b)}};
}
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
} // namespace

int main() {
  bool ok = true;
  rink::RinkMapDocument document;
  document.generation = "fixture-generation";
  document.calibration_revision = "fixture-revision";
  auto fitted = rink::FitRinkMap(document);
  ok &= expect(fitted.ok() && fitted->state == rink::RinkMapState::kPartial, "An empty map must remain partial");
  for (const char* id : {"center-line", "blue-left-line", "blue-right-line"}) {
    document.observations.push_back(line(document.layout, id));
    fitted = rink::FitRinkMap(document);
    ok &= expect(
        fitted.ok() && fitted->state == rink::RinkMapState::kPartial,
        "Centre and blue lines alone must never invent goal locations");
  }
  for (int i = 0; i < 10; ++i)
    document.observations.push_back(line(document.layout, "center-line"));
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kPartial, "Repeated frames cannot fill missing geometry");
  auto noisy = document;
  for (size_t i = 0; i < noisy.observations.size(); ++i) {
    auto& r = noisy.observations[i].rays[0];
    r += cv::Vec3d(0.01 * (int(i) % 3 - 1), 0.02, 0);
    r /= cv::norm(r);
  }
  fitted = rink::FitRinkMap(noisy);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kPartial,
      "Noisy repeated parallel lines must stay saveable as partial evidence");
  auto goal_lines = noisy;
  for (const char* id : {"goal-left-line", "goal-right-line"})
    goal_lines.observations.push_back(line(goal_lines.layout, id));
  fitted = rink::FitRinkMap(goal_lines);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kPartial,
      "Manual goal lines with detected parallel lines remain partial");
  for (const char* id : {"goal-left", "goal-right"}) {
    auto observation = point(goal_lines.layout, id);
    observation.rays[0] += cv::Vec3d(.005, -.004, .002);
    observation.rays[0] /= cv::norm(observation.rays[0]);
    goal_lines.observations.push_back(observation);
  }
  fitted = rink::FitRinkMap(goal_lines);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kPartial,
      "Noisy goal centres on one axis cannot supply missing cross-rink evidence");
  document.observations.push_back(line(document.layout, "near-boards"));
  document.observations.push_back(line(document.layout, "far-boards"));
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kReady, "Two board lines should complete a neutral-zone map");
  if (fitted.ok() && fitted->state == rink::RinkMapState::kReady) {
    for (const auto p : {cv::Point2d(-27.1272, 0), cv::Point2d(27.1272, 0), cv::Point2d(12, 7)}) {
      const auto mapped = rink::RinkRayToPoint(*fitted, ray(p));
      const auto projected = rink::RinkPointToRay(*fitted, p);
      ok &= expect(
          mapped.ok() && cv::norm(*mapped - p) < 1e-6 && projected.ok() && cv::norm(*projected - ray(p)) < 1e-6,
          "The map must recover goal centres through calibrated rays");
    }
    ok &= expect(!rink::RinkRayToPoint(*fitted, -ray({0, 0})).ok(), "A backwards ice-plane intersection must fail");
  }
  document.observations.clear();
  for (const char* id : {"blue-left-line", "blue-right-line", "far-boards"})
    document.observations.push_back(line(document.layout, id));
  document.observations.back().mask_revision = std::string(64, 'a');
  document.observations.back().automatic = true;
  document.observations.back().enabled = false;
  document.observations.push_back(point(document.layout, "center"));
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kPartial, "Unchecked mask suggestions cannot complete a map");
  document.observations[2].enabled = true;
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kReady,
      "Reviewed far boards, blue lines and centre ice can fit without the hidden near boards");
  const auto mask_saved = rink::SerializeRinkMap(document);
  const auto mask_loaded = mask_saved.ok() ? rink::ParseRinkMap(*mask_saved) : mask_saved.status();
  ok &= expect(
      mask_loaded.ok() && mask_loaded->observations[2].mask_revision == std::string(64, 'a'),
      "Mask observation provenance survives save/reopen");
  for (bool both_boards : {false, true}) {
    document.observations.clear();
    for (const char* id : {"goal-left-line", "goal-right-line", "far-boards"})
      document.observations.push_back(line(document.layout, id));
    document.observations.push_back(
        both_boards ? line(document.layout, "near-boards") : point(document.layout, "center"));
    fitted = rink::FitRinkMap(document);
    ok &= expect(
        fitted.ok() && fitted->state == rink::RinkMapState::kReady,
        "Goal-line completion suggestions supply enough independent geometry");
  }
  document.observations.clear();
  for (const char* id : {"goal-left-near", "goal-left-far", "goal-right-near", "goal-right-far"})
    document.observations.push_back(point(document.layout, id));
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kReady,
      "Four goal-line/board intersections locate the map without other landmarks");
  document.observations.clear();
  for (const char* id : {"center-near", "center-far", "blue-left-near", "blue-left-far"})
    document.observations.push_back(point(document.layout, id));
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kReady,
      "Four neutral-zone anchors must work without a single visible goal");
  const auto serialized = rink::SerializeRinkMap(document);
  auto restored = serialized.ok() ? rink::ParseRinkMap(*serialized) : serialized.status();
  ok &= expect(
      restored.ok() && restored->observations.size() == 4 && restored->generation == document.generation,
      "Save and reload must retain source evidence and calibration identity");
  auto skewed = document;
  for (auto& observation : skewed.observations) {
    auto p = *rink::RinkFeaturePoint(skewed.layout, observation.feature);
    p.x += 0.25 * p.y;
    observation.rays = {ray(p)};
  }
  fitted = rink::FitRinkMap(skewed);
  ok &= expect(
      fitted.ok() && fitted->state == rink::RinkMapState::kInconsistent,
      "An exact four-point fit with nonphysical rink axes must not publish goal estimates");
  auto saved_skewed = rink::SerializeRinkMap(skewed);
  auto restored_skewed = saved_skewed.ok() ? rink::ParseRinkMap(*saved_skewed) : saved_skewed.status();
  ok &= expect(
      restored_skewed.ok() && restored_skewed->observations.size() == skewed.observations.size() &&
          rink::FitRinkMap(*restored_skewed)->state == rink::RinkMapState::kInconsistent,
      "Rejected fits retain all evidence when saved as a partial map");
  auto contradictory = document;
  auto wrong = point(document.layout, "goal-right");
  wrong.rays = {ray({-20, 7})};
  contradictory.observations.push_back(wrong);
  fitted = rink::FitRinkMap(contradictory);
  ok &= expect(
      fitted.ok() && fitted->state != rink::RinkMapState::kReady, "Contradictory marks cannot produce a reviewed map");
  document.observations[3].rays = document.observations[0].rays;
  fitted = rink::FitRinkMap(document);
  ok &= expect(
      fitted.ok() && fitted->state != rink::RinkMapState::kReady, "Collapsed geometry must not produce goal estimates");
  document.observations[0].rays[0][0] = std::nan("");
  ok &= expect(!rink::FitRinkMap(document).ok(), "Nonfinite rays must fail");
  ok &= expect(!rink::ParseRinkMap(std::string(600000, 'x')).ok(), "Oversized maps must fail");
  auto invalid_layout = rink::RinkLayout{};
  invalid_layout.blue_line_distance = 100;
  ok &= expect(!rink::ValidateRinkLayout(invalid_layout).ok(), "Impossible dimensions must fail");
  if (ok)
    std::cout << "RinkMap tests passed\n";
  return ok ? 0 : 1;
}
