#include "hstream/src/libs/scoreboard/Scoreboard.h"

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include <iostream>
#include <vector>

int main() {
  cv::Mat input(32, 48, CV_8UC3);
  for (int y = 0; y < input.rows; ++y) {
    for (int x = 0; x < input.cols; ++x) {
      const unsigned char value = (x + y) % 2 == 0 ? 0 : 255;
      input.at<cv::Vec3b>(y, x) = cv::Vec3b(value, x * 5, y * 7);
    }
  }
  const std::vector<cv::Point2f> points{{5.25f, 4.5f}, {36.5f, 6.25f}, {34.25f, 24.5f}, {6.5f, 22.25f}};
  for (const double scale : {0.5, 1.0, 2.0}) {
    const int width = 32 * scale, height = 20 * scale;
    hm::scoreboard::Scoreboard<uchar3> scoreboard(points, width, height, false);
    const std::vector<cv::Point2f> target{
        {0, 0}, {float(width - 1), 0}, {float(width - 1), float(height - 1)}, {0, float(height - 1)}};
    cv::Mat expected;
    cv::warpPerspective(
        input, expected, cv::getPerspectiveTransform(points, target), cv::Size(width, height), cv::INTER_LINEAR);
    const auto actual = scoreboard.forward_cv(input);
    if (actual.size() != expected.size() || cv::norm(actual, expected, cv::NORM_INF) > 1) {
      std::cerr << "Scoreboard must sample the original pixels once at scale " << scale << '\n';
      return 1;
    }
  }
  // Integer boundary corners must survive extraction, including the last row
  // and column. A one-pixel identity mapping must not soften the LED grid.
  const std::vector<cv::Point2f> corners{{5, 4}, {36, 4}, {36, 23}, {5, 23}};
  hm::scoreboard::Scoreboard<uchar3> identity(corners, 32, 20, false);
  if (cv::norm(identity.forward_cv(input), input(cv::Rect(5, 4, 32, 20)), cv::NORM_INF) != 0) {
    std::cerr << "Scoreboard boundary pixels or identity mapping changed\n";
    return 1;
  }
  return 0;
}
