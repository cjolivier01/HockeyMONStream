#include "hstream/src/libs/stitching/RinkMaskGeometry.h"

#include <opencv2/imgproc.hpp>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace {
void check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
std::optional<cv::Vec3d> ray(double x, double y) {
  cv::Vec3d r((x - 0.5) * 2, (y - 0.5), 1);
  return r / cv::norm(r);
}
cv::Mat mask(bool obstruction) {
  cv::Mat result(400, 800, CV_8UC1, cv::Scalar(0));
  for (int x = 30; x < 770; ++x) {
    const int bottom = obstruction ? 240 + int(50 * std::sin(x * .035)) : 390;
    for (int y = 100; y < bottom; ++y)
      result.at<uchar>(y, x) = 255;
  }
  return result;
}
std::optional<cv::Vec3d> panoramaRay(double x, double y) {
  const double yaw = (x - .5) * 2, pitch = (y - .5) * 1.2;
  return cv::Vec3d(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
}
cv::Mat nearMask(bool interrupted) {
  cv::Mat result(400, 800, CV_8UC1, cv::Scalar(0));
  for (int x = 30; x < 770; ++x) {
    // A straight physical board is curved in the equirectangular mask.
    const double yaw = ((x + .5) / 800 - .5) * 2;
    int bottom = int((std::atan(.5 * std::cos(yaw)) / 1.2 + .5) * 400);
    if (interrupted) {
      if (x >= 390 && x < 440)
        bottom += 30; // Open bench door: ice extends below the usual boundary.
      if (x >= 260 && x < 290)
        bottom -= 75; // Foreground obstruction.
    }
    for (int y = 80; y < bottom; ++y)
      result.at<uchar>(y, x) = 255;
  }
  return result;
}
} // namespace
int main() {
  try {
    const auto clean = hm::stitching::SuggestFarRinkBoard(mask(false), ray);
    const auto obstructed = hm::stitching::SuggestFarRinkBoard(mask(true), ray);
    check(clean.ok() && obstructed.ok(), "A broad visible far boundary is proposed");
    check(clean->rays == obstructed->rays, "Near board tops and posts cannot move the far-board proposal");
    auto full = mask(false);
    full.setTo(255);
    check(!hm::stitching::SuggestFarRinkBoard(full, ray).ok(), "An image crop is not a board");
    auto broken = mask(false);
    cv::rectangle(broken, {300, 90}, {430, 180}, cv::Scalar(0), cv::FILLED);
    check(!hm::stitching::SuggestFarRinkBoard(broken, ray).ok(), "Large far-side occlusions are rejected");
    auto curved = mask(false);
    curved.setTo(0);
    for (int x = 30; x < 770; ++x)
      for (int y = 60 + int((x - 400) * (x - 400) * .0015); y < 390; ++y)
        curved.at<uchar>(y, x) = 255;
    check(!hm::stitching::SuggestFarRinkBoard(curved, ray).ok(), "A rounded end is not a straight board");
    check(
        !hm::stitching::SuggestFarRinkBoard(
             mask(false), [](double x, double y) { return x > .4 && x < .6 ? std::nullopt : ray(x, y); })
             .ok(),
        "Missing projection coverage cannot bridge a board");
    check(!hm::stitching::SuggestFarRinkBoard(cv::Mat(32, 4096, CV_8UC1), ray).ok(), "Mask analysis is bounded");
    const auto near = hm::stitching::SuggestNearRinkBoard(nearMask(false), panoramaRay);
    const auto interrupted = hm::stitching::SuggestNearRinkBoard(nearMask(true), panoramaRay);
    check(near.ok() && interrupted.ok(), "Near-board arc bridges door openings and foreground obstructions");
    const auto plane = near->rays.front().cross(near->rays.back());
    for (size_t i = 0; i < near->rays.size(); ++i) {
      check(cv::norm(near->rays[i] - interrupted->rays[i]) < .003, "Local interruptions do not pull the arc");
      check(std::abs(plane.dot(near->rays[i])) < 1e-10, "The proposed arc stays smooth through interruptions");
    }
    check(
        hm::stitching::SuggestNearRinkBoard(
            nearMask(true), [](double x, double y) { return x > .45 && x < .53 ? std::nullopt : panoramaRay(x, y); })
            .ok(),
        "A small missing near-side projection section can be bridged");
    check(!hm::stitching::SuggestNearRinkBoard(full, ray).ok(), "A clipped near edge cannot become an arc");
    check(!hm::stitching::SuggestNearRinkBoard(mask(true), ray).ok(), "Wild lower contours cannot become an arc");
    check(
        !hm::stitching::SuggestNearRinkBoard(
             nearMask(false), [](double x, double y) { return x > .37 && x < .63 ? std::nullopt : panoramaRay(x, y); })
             .ok(),
        "Isolated end patches cannot supply a near-board arc");
    std::cout << "RinkMaskGeometry tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
