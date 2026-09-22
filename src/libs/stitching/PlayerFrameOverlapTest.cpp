#include "hstream/src/libs/stitching/PlayerFrameOverlap.h"

#include <cmath>
#include <iostream>
#include <limits>

using namespace hm::stitching;
namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}
PlayerFrameRemap map(int width, int height, double x, double y) {
  return {
      cv::Mat(height, width, CV_16UC1, cv::Scalar(1)),
      cv::Mat(height, width, CV_16UC1, cv::Scalar(1)),
      {x, y},
      {100, 100}};
}
} // namespace

int main() {
  bool ok = true;
  std::array<PlayerFrameRemap, 2> maps{map(8, 8, -3, 5), map(8, 8, 1, 5)};
  auto overlap = BuildPlayerFrameOverlap(maps, {12, 8}, 0, 100);
  ok &= expect(overlap.ok(), "placed maps build overlap");
  if (!overlap.ok())
    return 1;
  ok &= expect(
      cv::countNonZero(overlap->mask) == 32 && overlap->mask.at<uint8_t>(3, 4) && !overlap->mask.at<uint8_t>(3, 3) &&
          !overlap->mask.at<uint8_t>(3, 8),
      "overlap uses normalized actual placement");
  // Transparent/unmapped holes are represented by invalid remaps, not TIFF rectangle extents.
  maps[0].x.at<uint16_t>(3, 5) = 65535;
  maps[1].y.at<uint16_t>(4, 2) = 65535;
  maps[1].x.at<uint16_t>(1, 1) = 100; // First out-of-source coordinate.
  overlap = BuildPlayerFrameOverlap(maps, {12, 8}, 0, 100);
  ok &= expect(
      overlap.ok() && cv::countNonZero(overlap->mask) == 29 && !overlap->mask.at<uint8_t>(3, 5) &&
          !overlap->mask.at<uint8_t>(4, 6) && !overlap->mask.at<uint8_t>(1, 5),
      "X/Y sentinel holes and out-of-camera mappings are excluded");
  auto reduced = BuildPlayerFrameOverlap(maps, {6, 4}, 0, 100);
  ok &= expect(
      reduced.ok() && reduced->mask.size() == cv::Size(6, 4) && !reduced->mask.at<uint8_t>(1, 2) &&
          !reduced->mask.at<uint8_t>(2, 3),
      "holes survive conservative effective-canvas downscaling");
  auto bounded = BuildPlayerFrameOverlap(maps, {12, 8}, 0, 3);
  ok &= expect(bounded.ok() && bounded->mask.cols <= 3 && bounded->mask.rows <= 3, "CPU mask dimension is bounded");
  // TIFF rational tags decode to floats just above an integer. Production
  // normalizes and measures in float; double arithmetic incorrectly loses a
  // pixel when the resulting extent is truncated to an integer.
  const std::array<PlayerFrameRemap, 2> fractional_positions{
      map(8, 8, 1.0000001192092896, 0), map(8, 8, 5, 0)};
  auto rounded = BuildPlayerFrameOverlap(fractional_positions, {12, 8}, 0, 100);
  ok &= expect(
      rounded.ok() && cv::countNonZero(rounded->mask) == 32,
      "fractional TIFF positions must use the same float normalization as production");
  const std::array<PlayerFrameRemap, 2> mini_positions{
      map(8, 8, 1207.0001220703125, 3581), map(8, 8, 5601, 3581)};
  ok &= expect(
      BuildPlayerFrameOverlap(mini_positions, {4402, 8}, 0, 100).ok(),
      "real mini TIFF placement rounding must not reject a validated canvas");
  std::array<PlayerFrameRemap, 2> square{map(9, 9, 0, 0), map(9, 9, 0, 0)};
  square[0].x.at<uint16_t>(2, 5) = 65535;
  auto rotated = BuildPlayerFrameOverlap(square, {9, 9}, 90, 9);
  ok &= expect(
      rotated.ok() && !rotated->mask.at<uint8_t>(3, 2) && rotated->mask.at<uint8_t>(4, 4),
      "positive ninety-degree rotation uses the production inverse affine and fixed center");
  auto fractional = BuildPlayerFrameOverlap(square, {9, 9}, 13, 9);
  ok &= expect(
      fractional.ok() && cv::countNonZero(fractional->mask) < 80 && !fractional->mask.at<uint8_t>(0, 0),
      "interpolated rotation boundaries are conservative");
  square[0].x.setTo(65535);
  auto empty = BuildPlayerFrameOverlap(square, {9, 9}, 0, 9);
  ok &= expect(empty.ok() && cv::countNonZero(empty->mask) == 0, "fully invalid camera gives empty shared visibility");
  ok &= expect(!BuildPlayerFrameOverlap(maps, {6, 8}, 0).ok(), "anisotropic canvas change rejected");
  ok &= expect(
      !BuildPlayerFrameOverlap(maps, {12, 8}, std::numeric_limits<double>::infinity()).ok(),
      "nonfinite rotation rejected");
  maps[0].y = cv::Mat(2, 2, CV_16UC1);
  ok &= expect(!BuildPlayerFrameOverlap(maps, {12, 8}, 0).ok(), "inconsistent X/Y dimensions rejected");
  return ok ? 0 : 1;
}
