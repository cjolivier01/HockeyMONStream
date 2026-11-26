#include "hstream/src/libs/scoreboard/CameraRegion.h"
#include "hstream/src/libs/scoreboard/ScoreboardPlan.h"

#include <opencv2/imgproc.hpp>

#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using Polygon = std::array<std::array<float, 2>, 4>;
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
cv::Mat Warp(const cv::Mat& source, const hm::scoreboard::Plan& plan) {
  cv::Mat output;
  cv::Mat inverse(3, 3, CV_32F, const_cast<float*>(plan.inverse));
  cv::warpPerspective(source, output, inverse, {plan.width, plan.height}, cv::INTER_LINEAR | cv::WARP_INVERSE_MAP);
  return output;
}
void Equivalent(const cv::Mat& source, const Polygon& polygon, int alignment) {
  const auto region = hm::scoreboard::MakeCameraRegion(polygon, source.cols, source.rows, alignment);
  Check(region.ok(), "valid camera region rejected");
  Check(
      region->left % alignment == 0 && region->top % alignment == 0 && region->width % alignment == 0 &&
          region->height % alignment == 0,
      "crop violates backend alignment");
  Check(
      region->left >= 0 && region->top >= 0 && region->left + region->width <= source.cols &&
          region->top + region->height <= source.rows,
      "crop exceeds original frame");
  const auto crop = source(cv::Rect(region->left, region->top, region->width, region->height));
  for (const auto flip :
       {hm::scoreboard::Orientation{},
        hm::scoreboard::Orientation{true, false},
        hm::scoreboard::Orientation{false, true},
        hm::scoreboard::Orientation{true, true}}) {
    const auto full = hm::scoreboard::MakePlan(
        polygon, source.cols, source.rows, 640, 480, 160, 80, hm::scoreboard::Position::kUpperLeft, flip);
    const auto cropped = hm::scoreboard::MakePlan(
        region->polygon, crop.cols, crop.rows, 640, 480, 160, 80, hm::scoreboard::Position::kUpperLeft, flip);
    Check(full.ok() && cropped.ok(), "valid crop changed perspective plan validity");
    Check(full->width == cropped->width && full->height == cropped->height, "crop changed overlay aspect");
    // OpenCV's table-based bilinear sampler may round the translated matrix
    // to an adjacent 1/32-pixel bin. The smooth fixture bounds that to one code.
    Check(
        cv::norm(Warp(source, *full), Warp(crop, *cropped), cv::NORM_INF) <= 1,
        "cropped overlay differs from full-frame reference");
  }
}
void PlaneViews() {
  using hm::gpu::PixelFormat;
  const Polygon polygon{{{10, 12}, {29, 12}, {29, 31}, {10, 31}}};
  const auto region = hm::scoreboard::MakeCameraRegion(polygon, 64, 48, 2);
  Check(region.ok(), "plane fixture region rejected");
  for (const auto format :
       {PixelFormat::kGray8,
        PixelFormat::kRgb8,
        PixelFormat::kRgba8,
        PixelFormat::kRgb10A2,
        PixelFormat::kRgba16Float,
        PixelFormat::kNv12,
        PixelFormat::kP010}) {
    const bool yuv = format == PixelFormat::kNv12 || format == PixelFormat::kP010;
    const size_t bytes = format == PixelFormat::kGray8 || format == PixelFormat::kNv12 ? 1
        : format == PixelFormat::kP010                                                 ? 2
        : format == PixelFormat::kRgb8                                                 ? 3
        : format == PixelFormat::kRgba16Float                                          ? 8
                                                                                       : 4;
    const size_t pitch = 64 * bytes + 64;
    std::vector<uint8_t> first(pitch * 48), second(pitch * 24);
    const hm::gpu::Image input{
        first.data(),
        yuv ? second.data() : nullptr,
        pitch,
        yuv ? pitch : 0,
        64,
        48,
        format,
        16,
        128,
        1.164f,
        1.793f,
        -.213f,
        -.533f,
        2.112f};
    const auto cropped = hm::scoreboard::CropCameraImage(input, *region);
    Check(cropped.ok(), "valid pixel format crop rejected");
    Check(
        cropped->plane0 == first.data() + 12 * pitch + 10 * bytes && cropped->pitch0 == pitch && cropped->width == 20 &&
            cropped->height == 20,
        "cropping changed pitch or misplaced first plane");
    if (yuv) {
      Check(
          cropped->plane1 == second.data() + 6 * pitch + 10 * bytes && cropped->pitch1 == pitch,
          "cropping misplaced subsampled chroma");
      Check(
          cropped->y_offset == input.y_offset && cropped->r_v == input.r_v && cropped->b_u == input.b_u,
          "cropping lost source colorimetry");
      auto unaligned = *region;
      unaligned.left += 1;
      Check(!hm::scoreboard::CropCameraImage(input, unaligned).ok(), "unaligned chroma crop accepted");
    }
    auto outside = *region;
    outside.left = 63;
    Check(!hm::scoreboard::CropCameraImage(input, outside).ok(), "out-of-bounds plane view accepted");
    outside.left = std::numeric_limits<int>::max();
    Check(!hm::scoreboard::CropCameraImage(input, outside).ok(), "overflowing crop accepted");
  }
}
} // namespace
int main() {
  try {
    PlaneViews();
    cv::Mat source(480, 640, CV_8UC3);
    for (int y = 0; y < source.rows; ++y)
      for (int x = 0; x < source.cols; ++x)
        source.at<cv::Vec3b>(y, x) = cv::Vec3b(x / 3, y / 2, (x + y) / 5);
    const Polygon fractional{{{101.25f, 31.75f}, {311.5f, 43.25f}, {307.75f, 126.5f}, {105.5f, 130.25f}}};
    for (int alignment : {1, 2, 4, 16}) {
      Equivalent(source, fractional, alignment);
      Equivalent(source, {{{0, 0}, {639, 0}, {639, 479}, {0, 479}}}, alignment);
      Equivalent(source, {{{501, 401}, {639, 401}, {639, 479}, {501, 479}}}, alignment);
    }
    const auto small = hm::scoreboard::MakeCameraRegion(fractional, 640, 480, 2);
    Check(
        small.ok() && small->width * small->height < 640 * 480 / 8,
        "scoreboard crop retained the full-frame processing footprint");
    for (int alignment : {-1, 0, 3, 641})
      Check(!hm::scoreboard::MakeCameraRegion(fractional, 640, 480, alignment).ok(), "bad alignment admitted");
    Check(!hm::scoreboard::MakeCameraRegion(fractional, 0, 480, 2).ok(), "zero frame admitted");
    Check(!hm::scoreboard::MakeCameraRegion(fractional, 639, 480, 2).ok(), "unaligned frame admitted");
    for (float invalid :
         {-1.f, 640.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
      auto polygon = fractional;
      polygon[0][0] = invalid;
      Check(!hm::scoreboard::MakeCameraRegion(polygon, 640, 480, 2).ok(), "out-of-frame coordinate admitted");
    }
    Check(!hm::scoreboard::MakeCameraRegion({}, 640, 480, 2).ok(), "degenerate polygon admitted");
    auto crossed = fractional;
    std::swap(crossed[1], crossed[2]);
    Check(!hm::scoreboard::MakeCameraRegion(crossed, 640, 480, 2).ok(), "crossed polygon admitted");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
