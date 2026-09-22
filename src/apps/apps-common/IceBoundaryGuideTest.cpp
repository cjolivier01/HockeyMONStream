#include "hstream/src/apps/apps-common/IceBoundaryGuide.h"
#include "hstream/src/libs/common/IceBoundary.h"

#include <chrono>
#include <iostream>

int main() {
  cv::Mat mask(100, 100, CV_8UC1, cv::Scalar(0));
  mask(cv::Rect(20, 20, 60, 60)).setTo(255);
  if (hm::fieldmask::inset_rink_mask(mask, {}).data != mask.data) {
    std::cerr << "Zero offsets must reuse the original mask\n";
    return 1;
  }
  const auto inset = hm::fieldmask::inset_rink_mask(mask, {5, 7, 3, 9});
  if (cv::boundingRect(inset) != cv::Rect(23, 25, 48, 48) || cv::countNonZero(inset) != 48 * 48 ||
      cv::countNonZero(mask) != 60 * 60) {
    std::cerr << "Each positive inset must exclude only its named side without mutating the original\n";
    return 2;
  }
  const auto expanded = hm::fieldmask::inset_rink_mask(mask, {-5, -7, -3, -9});
  if (cv::boundingRect(expanded) != cv::Rect(17, 15, 72, 72) || cv::countNonZero(expanded) != 72 * 72)
    return 3;
  const auto mixed = hm::fieldmask::inset_rink_mask(mask, {5, -7, -3, 9});
  if (cv::boundingRect(mixed) != cv::Rect(17, 25, 54, 62))
    return 4;
  if (cv::countNonZero(hm::fieldmask::inset_rink_mask(mask, {100, 0, 0, 0})) != 0)
    return 5;

  const hm::fieldmask::IceBoundaryOffsets samples{-0.1F, 0.1F, 0.2F, 0.2F};
  const auto far = hm::fieldmask::ice_boundary_samples(30, 40, 20, 20, {50, 50}, samples);
  const auto near = hm::fieldmask::ice_boundary_samples(70, 80, 20, 20, {50, 50}, samples);
  if (!far.uses_feet || far.feet.x != 32 || far.feet.y != 38 || far.center.x != 30 || far.center.y != 32 ||
      near.uses_feet || near.feet.x != 68 || near.feet.y != 78 || near.center.x != 70 || near.center.y != 72) {
    std::cerr << "Markers must represent the same foot/center points and branch as pruning\n";
    return 6;
  }

  cv::Mat large(2160, 7680, CV_8UC1, cv::Scalar(0));
  cv::ellipse(large, {3840, 1080}, {3500, 800}, 0, 0, 360, cv::Scalar(255), -1);
  const auto start = std::chrono::steady_clock::now();
  const auto adjusted = hm::fieldmask::inset_rink_mask(large, {20, -10, 15, -5});
  const auto bounded = hm::gpu_preview::bounded_rink_mask(adjusted);
  const auto contours = hm::gpu_preview::rink_mask_contours(bounded, large.cols, large.rows);
  if (bounded.cols > 2048 || bounded.rows > 2048 || contours.empty() || contours.front().size() < 20)
    return 7;
  std::cout << "7680x2160 mask adjustment plus bounded overlay: "
            << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << " ms\n";
  return 0;
}
