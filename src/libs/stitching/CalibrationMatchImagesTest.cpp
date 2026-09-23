#include "hstream/src/libs/stitching/CalibrationMatchImages.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {
bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << message << '\n';
  return condition;
}
bool green(const cv::Mat& image, int x, int y) {
  const auto pixel = image.at<cv::Vec3b>(y, x);
  return pixel[1] > 200 && pixel[1] > pixel[0] + 100 && pixel[1] > pixel[2] + 100;
}
} // namespace

int main() {
  using namespace hm::stitching;
  bool ok = true;
  const cv::Mat left(1024, 2048, CV_16UC3, cv::Scalar::all(32768));
  const cv::Mat right(300, 200, CV_8UC1, cv::Scalar::all(64));
  const std::vector<FeatureMatch> matches{{{1024, 512}, {100, 100}, 0.9f}};
  const auto images = MakeCalibrationMatchImages({left, right}, matches);
  ok &= expect(images.ok(), "Mixed-size and mixed-depth camera stills must render");
  if (images.ok()) {
    const auto& points = (*images)[0];
    const auto& lines = (*images)[1];
    ok &=
        expect(points.size() == cv::Size(1224, 512), "Diagnostic stills must remain bounded without enlarging inputs");
    ok &= expect(points.at<cv::Vec3b>(0, 0) == cv::Vec3b(128, 128, 128), "16-bit stills must preserve midtones");
    ok &= expect(points.at<cv::Vec3b>(0, 1024) == cv::Vec3b(64, 64, 64), "Grayscale inputs must become BGR");
    ok &= expect(
        green(points, 512, 256) && green(points, 1124, 100),
        "Endpoints must scale separately and offset the right camera");
    ok &= expect(
        !green(points, 818, 178) && green(lines, 818, 178),
        "Match lines must connect the scaled endpoints only in the match view");
    ok &=
        expect(points.at<cv::Vec3b>(400, 1100) == cv::Vec3b(0, 0, 0), "Shorter camera stills must have black padding");
  }
  AkazeMatchingCalibration calibration;
  calibration.left = FisheyeLensCalibration{cv::Size(200, 200), 100, 100, 100, 100, {0, 0, 0, 0}};
  const cv::Mat raw(400, 400, CV_8UC3, cv::Scalar::all(0));
  const auto fisheye = MakeCalibrationMatchImages({raw, raw}, {{{360, 200}, {100, 100}, 1}}, calibration);
  const int distorted_x = cvRound(200 + 200 * std::atan(0.8));
  ok &= expect(
      fisheye.ok() && green((*fisheye)[0], distorted_x, 200) && !green((*fisheye)[0], 360, 200),
      "Calibrated AKAZE endpoints must return to raw camera coordinates using scaled lens intrinsics");
  ok &=
      expect(MakeCalibrationMatchImages({left, right}, {}).ok(), "An empty match set must render plain camera stills");
  ok &= expect(!MakeCalibrationMatchImages({cv::Mat{}, right}, matches).ok(), "Missing images must fail explicitly");
  ok &= expect(
      !MakeCalibrationMatchImages({left, right}, {{{10, 20}, {200, 20}, 1}}).ok(), "Out-of-image matches must fail");
  ok &= expect(
      !MakeCalibrationMatchImages({left, right}, {{{std::numeric_limits<float>::quiet_NaN(), 20}, {20, 20}, 1}}).ok(),
      "Non-finite matches must fail");
  return ok ? 0 : 1;
}
