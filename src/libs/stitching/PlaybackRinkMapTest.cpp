#include "hstream/src/libs/stitching/PlaybackRinkMap.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"

#include <unistd.h>
#include <cmath>
#include <iostream>

namespace {
namespace rink = hm::stitching;
bool expect(bool value, const char* message) {
  if (!value)
    std::cerr << "FAIL: " << message << '\n';
  return value;
}
constexpr double pi = 3.14159265358979323846;
const cv::Matx33d camera(0, -1, 19, -1, 0, -3, 0, 0, -7);
cv::Point2d normalized(cv::Point2d point) {
  auto ray = camera * cv::Vec3d(point.x, point.y, 1);
  ray /= cv::norm(ray);
  return {
      (1800 - std::atan2(ray[1], ray[0]) * 1800 / pi - 900) / 1800, (900 - std::asin(ray[2]) * 1800 / pi - 300) / 1200};
}
} // namespace

int main(int argc, char** argv) {
  if (argc == 3 && std::string(argv[1]) == "--load-game") {
    auto map = rink::LoadPlaybackRinkMap(argv[2]);
    if (!map.ok()) {
      std::cerr << map.status() << '\n';
      return 1;
    }
    std::cout << "Playback map ready: " << (*map)->rays.size() << " projection rays\n";
    return 0;
  }
  bool ok = true;
  rink::PlaybackRinkMap map;
  map.hugin_generation = "fixture\n";
  map.fit.state = rink::RinkMapState::kReady;
  map.fit.plane_to_ray = camera;
  const auto generation = rink::stitched_output_generation_id(map.hugin_generation, 23.5, 1800, 1200);
  ok &=
      expect(generation.ok() && map.OutputRotation(*generation, 1800, 1200) == 23.5, "Frame rotation is authoritative");
  ok &= expect(!map.OutputRotation(*generation, 900, 600), "Stale output dimensions are rejected");
  auto stale = rink::stitched_output_generation_id("other\n", 0, 1800, 1200);
  ok &= expect(!map.OutputRotation(*stale, 1800, 1200), "Stale calibration is rejected");
  ok &= expect(!map.OutputRotation("malformed", 1800, 1200), "Malformed generation is rejected");
  map.rays.resize(map.columns * map.rows);
  for (size_t y = 0; y < map.rows; ++y)
    for (size_t x = 0; x < map.columns; ++x) {
      const double longitude = (900 + double(x) * 1800 / (map.columns - 1) - 1800) * pi / 1800;
      const double latitude = (900 - 300 - double(y) * 1200 / (map.rows - 1)) * pi / 1800;
      map.rays[y * map.columns + x] = {
          std::cos(latitude) * std::cos(longitude), -std::cos(latitude) * std::sin(longitude), std::sin(latitude)};
    }
  for (auto point : {cv::Point2d(0, 0), cv::Point2d(-25, 0), cv::Point2d(25, 0), cv::Point2d(0, 10)}) {
    const auto p = normalized(point);
    auto located = map.Locate(p.x, p.y, 0, 1800, 1200);
    ok &= expect(located && cv::norm(*located - point) < 0.015, "Cropped projection preserves rink positions");
    for (double degrees : {-25.0, 12.0}) {
      const double angle = degrees * pi / 180, dx = (p.x - 0.5) * 1800, dy = (p.y - 0.5) * 1200;
      const double x = .5 + (std::cos(angle) * dx + std::sin(angle) * dy) / 1800;
      const double y = .5 + (-std::sin(angle) * dx + std::cos(angle) * dy) / 1200;
      auto rotated = map.Locate(x, y, degrees, 900, 600);
      ok &= expect(
          rotated && cv::norm(*rotated - point) < 0.015, "Post-stitch rotation and scaling preserve player locations");
    }
  }
  auto outside = normalized({30, 12});
  ok &= expect(!map.Locate(outside.x, outside.y, 0, 1800, 1200), "Rounded board corners exclude off-ice players");
  ok &= expect(!map.Locate(-.1, .5, 0, 1800, 1200), "Off-image feet are rejected");
  map.fit.state = rink::RinkMapState::kPartial;
  ok &= expect(!map.Locate(.5, .5, 0, 1800, 1200), "Partial maps cannot locate players");
  map.fit.state = rink::RinkMapState::kReady;
  if (access("/usr/bin/pano_trafo", X_OK) == 0) {
    const std::string pto = "p f2 w3600 h1800 v360 S900,2700,300,1500 n\"TIFF_m\"\n";
    auto rays = rink::RinkProjectionRays(pto, 1800, 1200, map.columns, map.rows);
    ok &= expect(rays.ok(), rays.ok() ? "Hugin projection" : rays.status().ToString().c_str());
    if (rays.ok()) {
      double maximum = 0;
      for (size_t i = 0; i < rays->size(); ++i)
        maximum = std::max(maximum, cv::norm(rays->at(i) - map.rays[i]));
      ok &=
          expect(maximum < 1e-6, "Real Hugin projection agrees with independent equirectangular rays and crop offsets");
    }
    ok &= expect(!rink::RinkProjectionRays(pto, 1800, 900, 3, 3).ok(), "PTO crop/provenance disagreement is rejected");
    const std::string quoted = "p f2 w3600 h1800 v360 n\"TIFF_m S1,2,3,4 w1 h2\" S900,2700,300,1500\n";
    ok &= expect(
        rink::RinkProjectionRays(quoted, 1800, 1200, 3, 3).ok(),
        "Quoted output names must not override panorama fields");
    ok &= expect(
        !rink::RinkProjectionRays("p f2 w3600 h1800 v360 Sbad\n", 3600, 1800, 3, 3).ok(),
        "Malformed crop must not silently become an uncropped panorama");
  } else {
    std::cout << "SKIP: installed Hugin projection comparison\n";
  }
  if (ok)
    std::cout << "PlaybackRinkMap tests passed\n";
  return ok ? 0 : 1;
}
