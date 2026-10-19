#include "hstream/src/libs/playtracker_replay/RinkCameraGuide.h"

#include <cmath>
#include <iostream>

namespace replay = hm::playtracker_replay;
namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
std::shared_ptr<hm::stitching::PlaybackRinkMap> map() {
  auto result = std::make_shared<hm::stitching::PlaybackRinkMap>();
  result->fit.state = hm::stitching::RinkMapState::kReady;
  result->fit.plane_to_ray = cv::Matx33d(1, 0, 0, 0, 1, 0, 0, 0, 20);
  result->columns = 65;
  result->rows = 33;
  for (size_t y = 0; y < result->rows; ++y)
    for (size_t x = 0; x < result->columns; ++x)
      result->rays.push_back({70.0 * x / (result->columns - 1) - 35, 30.0 * y / (result->rows - 1) - 15, 20});
  return result;
}
hm::BBox box(double x, double y = 0) {
  const double px = (x + 35) * 100, py = (y + 15) * 100;
  return hm::BBox(px - 30, py - 140, px + 30, py);
}
double centre(const hm::BBox& box) {
  return (box.left + box.right) / 2;
}
bool same(const hm::BBox& a, const hm::BBox& b) {
  return a.left == b.left && a.right == b.right && a.top == b.top && a.bottom == b.bottom;
}
} // namespace

int main() {
  bool ok = true;
  const auto geometry = map();
  const hm::BBox arena(0, 0, 7000, 3000), baseline(2000, 700, 5000, 2300);
  const auto projected = geometry->Project({26, 0}, 0, 7000, 3000);
  ok &= expect(projected && std::abs(projected->x - 61.0 / 70) < 1e-6, "Static goal projects correctly");
  for (int fps : {15, 30, 60}) {
    replay::RinkCameraGuide guide({geometry, {true, false, true}}, 0, 7000, 3000);
    replay::RinkCameraGuide off({geometry, {false, false, false}}, 0, 7000, 3000);
    replay::RinkCameraInfo info, ignored;
    hm::BBox output = baseline;
    std::array<replay::RinkCameraGuide::Track, 5> tracks;
    uint64_t now = 0;
    for (int frame = 0; frame <= fps * 2; ++frame) {
      now = uint64_t(frame) * 1000000000 / fps;
      const double x = -8 + frame * 6.0 / fps;
      for (size_t i = 0; i < tracks.size(); ++i)
        tracks[i] = {i + 1, box(x + (double(i) - 2) * 1.5), false};
      const hm::BBox cluster(tracks.front().box.left, 1200, tracks.back().box.right, 1600);
      output = guide.Step(now, baseline, arena, cluster, tracks.data(), tracks.size(), &info);
      ok &= expect(
          same(off.Step(now, baseline, arena, cluster, tracks.data(), tracks.size(), &ignored), baseline),
          "All guide options off preserve the native camera exactly");
      if (!frame)
        ok &= expect(same(output, baseline), "First frame preserves the historical starting camera");
    }
    ok &=
        expect(info.active && std::abs(info.speed_mps - 6) < 0.01, "Collective speed uses rink metres across cadences");
    ok &= expect(
        centre(output) > centre(baseline) + 200 && output.width() > baseline.width() * 1.1,
        "Rush leads play and widens framing");
    const auto duplicate = guide.Step(now, baseline, arena, {}, nullptr, 0, &ignored);
    ok &= expect(same(duplicate, output), "Repeated timestamp cannot advance framing");
    ok &= expect(
        same(guide.Step(now + 500000000, baseline, arena, {}, nullptr, 0, &info), baseline),
        "Timestamp gap resets prediction");
    ok &= expect(same(guide.Step(1, baseline, arena, {}, nullptr, 0, &info), baseline), "Seek resets prediction");
  }
  replay::RinkCameraGuide settled({geometry, {true, true, true}}, 0, 7000, 3000);
  replay::RinkCameraInfo info;
  std::array<replay::RinkCameraGuide::Track, 4> tracks;
  hm::BBox output;
  for (int frame = 0; frame < 180; ++frame) {
    for (size_t i = 0; i < tracks.size(); ++i)
      tracks[i] = {i + 1, box((double(i) - 1.5) * 1.5), false};
    output = settled.Step(
        uint64_t(frame) * 33333333,
        baseline,
        arena,
        hm::BBox(3100, 1300, 3900, 1600),
        tracks.data(),
        tracks.size(),
        &info);
  }
  ok &= expect(
      info.active && info.speed_mps < 0.01 && output.width() < baseline.width() * 0.96,
      "Settled play tightens gradually");
  replay::RinkCameraGuide goals({geometry, {false, true, false}}, 0, 7000, 3000);
  const hm::BBox end_baseline(4000, 700, 6000, 2300);
  for (int frame = 0; frame < 180; ++frame) {
    for (size_t i = 0; i < tracks.size(); ++i)
      tracks[i] = {i + 1, box(18 + i), false};
    output = goals.Step(
        uint64_t(frame) * 33333333,
        end_baseline,
        arena,
        hm::BBox(5250, 1300, 5650, 1600),
        tracks.data(),
        tracks.size(),
        &info);
  }
  ok &= expect(info.goal == 1 && output.right > 6200, "End-zone framing brings the nearby goal into view");
  replay::RinkCameraGuide framed_goal({geometry, {false, true, false}}, 0, 7000, 3000);
  const hm::BBox inclusive(4000, 500, 6800, 2400);
  for (int frame = 0; frame < 90; ++frame) {
    for (size_t i = 0; i < tracks.size(); ++i)
      tracks[i] = {i + 1, box(18 + i + std::sin(frame * 0.05)), false};
    output = framed_goal.Step(uint64_t(frame) * 33333333, inclusive, arena, arena, tracks.data(), tracks.size(), &info);
    ok &= expect(same(output, inclusive), "An already visible goal must not add camera movement");
  }
  const auto before = tracks;
  for (auto& track : tracks)
    track.ignored = true;
  replay::RinkCameraGuide excluded({geometry, {}}, 0, 7000, 3000);
  for (int frame = 0; frame < 90; ++frame)
    output = excluded.Step(uint64_t(frame) * 33333333, baseline, arena, arena, tracks.data(), tracks.size(), &info);
  ok &= expect(same(output, baseline) && info.located == 0, "Native exclusions do not steer the guide");
  for (size_t i = 0; i < tracks.size(); ++i)
    ok &= expect(same(tracks[i].box, before[i].box), "Guide leaves detection boxes unchanged");
  geometry->fit.state = hm::stitching::RinkMapState::kPartial;
  replay::RinkCameraGuide partial({geometry, {}}, 0, 7000, 3000);
  ok &= expect(
      same(partial.Step(1, baseline, arena, arena, tracks.data(), tracks.size(), &info), baseline),
      "Partial map retains native framing");
  return ok ? 0 : 1;
}
