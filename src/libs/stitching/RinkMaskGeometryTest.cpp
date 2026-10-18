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
    std::cout << "RinkMaskGeometry tests passed\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
