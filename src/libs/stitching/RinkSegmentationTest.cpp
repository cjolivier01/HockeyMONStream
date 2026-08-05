#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <vector>

#include <opencv2/core.hpp>

#include "absl/status/status.h"

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  bool ok = true;
  ok &= expect(!hm::stitching::RinkSegmentation::Prepare({}).ok(), "empty image must fail");
  cv::Mat wrong_type(2, 4, CV_32FC3);
  ok &= expect(!hm::stitching::RinkSegmentation::Prepare(wrong_type).ok(), "non-byte image must fail");

  cv::Mat bgr(2, 4, CV_8UC3, cv::Scalar(30, 20, 10));
  auto prepared = hm::stitching::RinkSegmentation::Prepare(bgr);
  ok &= expect(prepared.ok(), "valid BGR image must preprocess");
  if (prepared.ok()) {
    ok &= expect(
        prepared->resized_width == 1333 && prepared->resized_height == 667,
        "aspect-preserving resize must use MMDetection's 1333x800 bounds");
    ok &= expect(
        prepared->tensor.size() ==
            static_cast<size_t>(3) * hm::stitching::RinkSegmentation::kInputHeight *
                hm::stitching::RinkSegmentation::kInputWidth,
        "preprocessed tensor must use fixed NCHW canvas");
    const float expected_red = (10.0f - 123.675f) / 58.395f;
    ok &= expect(std::abs(prepared->tensor[0] - expected_red) < 1e-5f, "BGR must become normalized RGB");
    const size_t padded_pixel = static_cast<size_t>(799) * hm::stitching::RinkSegmentation::kInputWidth + 1343;
    ok &= expect(prepared->tensor[padded_pixel] == 0.0f, "padding must be zero in normalized space");
  }

  hm::stitching::RinkInput metadata;
  metadata.source_width = 64;
  metadata.source_height = 32;
  metadata.resized_width = 64;
  metadata.resized_height = 32;
  std::vector<float> classes(
      static_cast<size_t>(hm::stitching::RinkSegmentation::kQueryCount) *
          hm::stitching::RinkSegmentation::kClassCountWithBackground,
      -10.0f);
  classes[1] = 10.0f;
  std::vector<float> masks(
      static_cast<size_t>(hm::stitching::RinkSegmentation::kQueryCount) * hm::stitching::RinkSegmentation::kMaskHeight *
          hm::stitching::RinkSegmentation::kMaskWidth,
      -10.0f);
  for (int y = 0; y < 12; ++y) {
    for (int x = 0; x < 24; ++x) {
      masks[static_cast<size_t>(y) * hm::stitching::RinkSegmentation::kMaskWidth + x] = 10.0f;
    }
  }
  auto profile = hm::stitching::RinkSegmentation::Postprocess(
      metadata, classes.data(), classes.size(), masks.data(), masks.size());
  ok &= expect(profile.ok(), "valid synthetic rink outputs must postprocess");
  if (profile.ok()) {
    ok &= expect(profile->masks.size() == 1, "only the rink-class candidate must be retained");
    ok &= expect(
        profile->combined_mask.type() == CV_8U && profile->combined_mask.cols == 64 &&
            profile->combined_mask.rows == 32 && cv::countNonZero(profile->combined_mask) > 0,
        "combined binary mask must have source dimensions");
    ok &= expect(
        profile->scores.size() == 1 && profile->scores[0] > 0.99f,
        "classification and mask confidence must be combined");
    ok &= expect(
        std::isfinite(profile->centroid.x) && std::isfinite(profile->centroid.y), "contour centroid must be finite");
    ok &= expect(
        profile->combined_bbox.width > 0.0 && profile->combined_bbox.height > 0.0,
        "combined bbox must enclose the accepted mask");
  }
  ok &= expect(
      !hm::stitching::RinkSegmentation::Postprocess(metadata, nullptr, 0, masks.data(), masks.size()).ok(),
      "missing logits must fail");

  const std::vector<float> valid_classes = classes;
  const std::vector<float> valid_masks = masks;
  for (const float invalid : {
           std::numeric_limits<float>::quiet_NaN(),
           std::numeric_limits<float>::infinity(),
           -std::numeric_limits<float>::infinity(),
       }) {
    classes = valid_classes;
    classes.back() = invalid;
    const auto bad_classes = hm::stitching::RinkSegmentation::Postprocess(
        metadata, classes.data(), classes.size(), valid_masks.data(), valid_masks.size());
    ok &= expect(absl::IsDataLoss(bad_classes.status()), "every non-finite class logit must fail before sorting");

    masks = valid_masks;
    masks.back() = invalid;
    const auto bad_masks = hm::stitching::RinkSegmentation::Postprocess(
        metadata, valid_classes.data(), valid_classes.size(), masks.data(), masks.size());
    ok &=
        expect(absl::IsDataLoss(bad_masks.status()), "every non-finite mask logit must fail before OpenCV processing");
  }

  std::fill(classes.begin(), classes.end(), -10.0f);
  auto no_rink = hm::stitching::RinkSegmentation::Postprocess(
      metadata, classes.data(), classes.size(), masks.data(), masks.size());
  ok &= expect(!no_rink.ok(), "outputs without a confident rink class must fail");
  return ok ? 0 : 1;
}
