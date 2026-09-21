#include "hstream/src/libs/scoreboard/Scoreboard.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <vector>

namespace {

cv::Mat expected_scoreboard(const cv::Mat& input, int resize_interpolation) {
  const cv::Rect source_roi(1, 1, 3, 3);
  cv::Mat resized;
  cv::resize(input(source_roi), resized, cv::Size(6, 6), 0, 0, resize_interpolation);

  const std::vector<cv::Point2f> source_points = {
      {0.0f, 0.0f},
      {6.0f, 0.0f},
      {6.0f, 6.0f},
      {0.0f, 6.0f},
  };
  const std::vector<cv::Point2f> destination_points = {
      {0.0f, 0.0f},
      {5.0f, 0.0f},
      {5.0f, 5.0f},
      {0.0f, 5.0f},
  };
  cv::Mat expected;
  cv::warpPerspective(
      resized,
      expected,
      cv::getPerspectiveTransform(source_points, destination_points),
      cv::Size(6, 6),
      cv::INTER_LINEAR);
  return expected;
}

} // namespace

int main() {
  cv::Mat input(6, 6, CV_8UC3);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      const unsigned char value = (x + y) % 2 == 0 ? 0 : 255;
      input.at<cv::Vec3b>(y, x) = cv::Vec3b(value, value, value);
    }
  }

  const std::vector<cv::Point2f> scoreboard_points = {
      {1.0f, 1.0f},
      {4.0f, 1.0f},
      {4.0f, 4.0f},
      {1.0f, 4.0f},
  };
  hm::scoreboard::Scoreboard<uchar3> scoreboard(
      scoreboard_points, /*destWidth=*/6, /*destHeight=*/6, /*autoAspect=*/false);
  const cv::Mat actual = scoreboard.forward_cv(input);
  const cv::Mat linear = expected_scoreboard(input, cv::INTER_LINEAR);
  const cv::Mat nearest = expected_scoreboard(input, cv::INTER_NEAREST);

  if (cv::norm(actual, linear, cv::NORM_INF) != 0.0) {
    std::cerr << "Scoreboard resize does not use bilinear sampling\n";
    return 1;
  }
  if (cv::norm(actual, nearest, cv::NORM_INF) == 0.0) {
    std::cerr << "Synthetic LED grid does not distinguish bilinear from nearest sampling\n";
    return 1;
  }
  return 0;
}
