#include "hstream/src/libs/detection/RinkDetectionMask.h"

#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void Check(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
} // namespace
int main() {
  try {
    // A non-byte-aligned mask with a known ice rectangle; source is an ROI
    // so packing must honor a row stride larger than the visible width.
    cv::Mat storage(20, 40, CV_8UC1, cv::Scalar(0));
    auto mask = storage(cv::Rect(3, 0, 19, 20));
    mask(cv::Rect(3, 5, 13, 11)).setTo(255);
    const auto packed = hm::detection::convert_to_bit_mask(mask);
    cv::Rect bounds;
    const auto center = hm::detection::compute_centroid(mask, bounds);
    Check(center == cv::Point2f(9, 10), "mask centroid");
    Check(bounds == cv::Rect(3, 5, 12, 10), "preserve native field bounds");
    auto evaluate = [&](cv::Rect2f box, float raise = 0, float lower = 0) {
      return hm::detection::EvaluateRinkDetection(packed, mask.size(), center, box, {1, 1}, raise, lower);
    };
    Check(evaluate({7, 1, 4, 5}).keep, "upper player with feet on ice survives");
    Check(!evaluate({7, 0, 4, 3}).keep, "upper spectator with feet off ice is removed");
    Check(evaluate({7, 12, 4, 6}).keep, "lower player with center on ice survives even when feet are off");
    Check(!evaluate({7, 15, 4, 6}).keep, "lower spectator with center off ice is removed");
    Check(!evaluate({7, 1, 4, 5}, 0, .4f).keep, "configured feet adjustment affects pruning");
    Check(evaluate({7, 15, 4, 6}, .5f, 0).keep, "configured center adjustment affects pruning");
    Check(evaluate({0, 1, 5, 5}).keep, "left edge feet shift toward ice");
    Check(evaluate({13, 1, 5, 5}).keep, "right edge feet shift toward ice");
    const auto scaled =
        hm::detection::EvaluateRinkDetection(packed, mask.size(), center, {3.5f, .5f, 2, 2.5f}, {2, 2}, 0, 0);
    Check(scaled.keep, "pipeline coordinates scale into source mask");
    Check(!evaluate({0, 0, -1, 3}).keep, "invalid box rejected");
    Check(!evaluate({0, 0, 1, 3}, std::numeric_limits<float>::infinity(), 0).keep, "invalid ratios rejected");
    Check(
        !hm::detection::EvaluateRinkDetection(packed, {18, 19}, center, {7, 1, 4, 5}, {1, 1}, 0, 0).keep,
        "mismatched mask dimensions rejected");
    // Packed mask views also retain their own row stride.
    cv::Mat packed_storage(packed.rows, packed.cols + 4, CV_8UC1, cv::Scalar(0));
    auto packed_view = packed_storage(cv::Rect(2, 0, packed.cols, packed.rows));
    packed.copyTo(packed_view);
    Check(
        hm::detection::EvaluateRinkDetection(packed_view, mask.size(), center, {7, 1, 4, 5}, {1, 1}, 0, 0).keep,
        "noncontiguous packed rows");
    std::cout << "Shared rink detection mask checks passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
