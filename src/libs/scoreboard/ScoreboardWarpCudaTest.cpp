#include "hstream/src/libs/scoreboard/Scoreboard.h"

#include <opencv2/imgproc.hpp>

#include <iostream>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
NvBufSurfaceParams Surface(hm::CudaMat<uchar4>& image) {
  NvBufSurfaceParams params{};
  params.dataPtr = image.data();
  params.width = image.width();
  params.height = image.height();
  params.pitch = image.pitch();
  params.colorFormat = NVBUF_COLOR_FORMAT_RGBA;
  return params;
}
} // namespace

int main() {
  try {
    cv::Mat source(40, 64, CV_8UC4);
    for (int y = 0; y < source.rows; ++y)
      for (int x = 0; x < source.cols; ++x)
        source.at<cv::Vec4b>(y, x) = cv::Vec4b(x * 3, y * 5, (x + y) % 2 ? 240 : 10, 0);
    const std::vector<cv::Point2f> points{{5.25f, 4.5f}, {36.5f, 6.25f}, {34.25f, 24.5f}, {6.5f, 22.25f}};
    hm::CudaMat<uchar4> input(source);
    auto input_surface = Surface(input);
    cv::Mat opaque = source.clone();
    for (int y = 0; y < opaque.rows; ++y)
      for (int x = 0; x < opaque.cols; ++x)
        opaque.at<cv::Vec4b>(y, x)[3] = 255;
    for (const double scale : {0.5, 1.0, 2.0}) {
      const int width = 32 * scale, height = 20 * scale;
      const std::vector<cv::Point2f> target{
          {0, 0}, {float(width - 1), 0}, {float(width - 1), float(height - 1)}, {0, float(height - 1)}};
      cv::Mat expected;
      cv::warpPerspective(
          opaque, expected, cv::getPerspectiveTransform(points, target), cv::Size(width, height), cv::INTER_LINEAR);
      for (bool horizontal : {false, true}) {
        for (bool vertical : {false, true}) {
          hm::scoreboard::Scoreboard<uchar4> scoreboard(points, width, height, false, nullptr, {horizontal, vertical});
          cv::Mat oriented;
          if (horizontal || vertical)
            cv::flip(expected, oriented, horizontal && vertical ? -1 : horizontal ? 1 : 0);
          else
            oriented = expected;
          for (const auto position :
               {hm::scoreboard::Position::kUpperLeft,
                hm::scoreboard::Position::kUpperCenter,
                hm::scoreboard::Position::kUpperRight}) {
            cv::Mat background(64, 96, CV_8UC4, cv::Scalar(7, 11, 13, 255));
            hm::CudaMat<uchar4> output(background);
            auto output_surface = Surface(output);
            const auto status = scoreboard.forward_prod(&input_surface, &output_surface, true, nullptr, position);
            Check(status.ok(), status.ToString().c_str());
            Check(cudaDeviceSynchronize() == cudaSuccess, "Warp synchronization failed");
            const cv::Mat actual = output.download();
            const int left = hm::scoreboard::HorizontalOffset(position, 96, width);
            // OpenCV quantizes interpolation weights to 1/32, CUDA uses floats.
            Check(
                cv::norm(actual(cv::Rect(left, 0, width, height)), oriented, cv::NORM_INF) <= 8,
                "CUDA must sample original RGB once and ignore video alpha");
            oriented.copyTo(background(cv::Rect(left, 0, width, height)));
            Check(cv::norm(actual, background, cv::NORM_INF) <= 8, "Overlay changed pixels outside its target");
            const auto cached = scoreboard.forward_prod(&input_surface, &output_surface, false, nullptr, position);
            Check(cached.ok() && cudaDeviceSynchronize() == cudaSuccess, "Cached scoreboard reuse failed");
          }
        }
      }
    }
    Check(cv::norm(source, input.download(), cv::NORM_INF) == 0, "Scoreboard modified the shared panorama");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
