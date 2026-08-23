#include "hstream/src/libs/stitching/FeatureMatcher.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <vector>

#include <opencv2/core.hpp>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  bool ok = true;
  ok &= expect(
      hm::stitching::ParseControlPointMatcher("superpoint-lightglue").ok(),
      "HockeyMOM baseline matcher spelling must be accepted");
  ok &=
      expect(hm::stitching::ParseControlPointMatcher("superpoint").ok(), "HockeyMOM superpoint alias must be accepted");
  ok &= expect(
      !hm::stitching::ParseControlPointMatcher("dedode-lightglue").ok(),
      "unsupported matcher engines must still fail explicitly");
  ok &= expect(!hm::stitching::FeatureMatcher::Prepare({}, {}).ok(), "empty images must fail");
  cv::Mat left(90, 160, CV_8UC3, cv::Scalar(30, 20, 10));
  cv::Mat right(100, 100, CV_8UC3, cv::Scalar(60, 50, 40));
  auto prepared = hm::stitching::FeatureMatcher::Prepare(left, right);
  ok &= expect(prepared.ok(), "valid feature images must preprocess");
  if (prepared.ok()) {
    ok &= expect(
        prepared->resized_sizes[0] == cv::Size(1024, 576) && prepared->resized_sizes[1] == cv::Size(576, 576),
        "feature inputs must resize aspect-preservingly into 1024x576");
    ok &= expect(
        prepared->tensor.size() == static_cast<size_t>(2) * 3 * 576 * 1024, "feature tensor must contain an RGB pair");
    ok &= expect(std::abs(prepared->tensor[0] - 10.0f / 255.0f) < 1e-6f, "BGR must become RGB in [0,1]");
    const size_t second_image = static_cast<size_t>(3) * 576 * 1024;
    ok &= expect(
        std::abs(prepared->tensor[second_image] - 40.0f / 255.0f) < 1e-6f,
        "second image must use the same preprocessing");
    ok &= expect(prepared->tensor[second_image + 575 * 1024 + 900] == 0.0f, "aspect padding must remain zero");
  }

  hm::stitching::FeaturePairInput metadata;
  metadata.source_sizes[0] = {7680, 4320};
  metadata.source_sizes[1] = {7680, 4320};
  metadata.resized_sizes[0] = {1024, 576};
  metadata.resized_sizes[1] = {1024, 576};
  std::vector<float> keypoints(static_cast<size_t>(2) * 2048 * 2, 0.0f);
  auto set_keypoint = [&](int image, int index, float x, float y) {
    const size_t offset = (static_cast<size_t>(image) * 2048 + index) * 2;
    keypoints[offset] = x;
    keypoints[offset + 1] = y;
  };
  set_keypoint(0, 0, 10.0f, 30.0f);
  set_keypoint(0, 1, 20.0f, 10.0f);
  set_keypoint(0, 2, 30.0f, 20.0f);
  set_keypoint(1, 0, 40.0f, 35.0f);
  set_keypoint(1, 1, 50.0f, 15.0f);
  set_keypoint(1, 2, 60.0f, 25.0f);
  std::vector<int64_t> matches = {0, 0, 0, 0, 1, 1, 0, 2, 2};
  std::vector<float> scores = {0.9f, 0.8f, 0.7f};
  auto result = hm::stitching::FeatureMatcher::Postprocess(
      metadata, keypoints.data(), keypoints.size(), matches.data(), matches.size(), scores.data(), scores.size(), 5);
  ok &= expect(result.ok(), "valid matches must postprocess");
  if (result.ok()) {
    ok &= expect(
        result->accepted_match_count == 3 && result->accepted.size() == 3 && result->selected.size() == 3,
        "maximum control-point count must cap rather than duplicate matches");
    ok &= expect(
        result->selected[0].left.y < result->selected.back().left.y,
        "selected control points must be ordered evenly by left Y");
    ok &= expect(
        std::abs(result->selected[0].left.x - 153.25f) < 1e-4f, "inverse resize must preserve half-pixel centers");
    ok &= expect(
        result->accepted[0].left_index == 0 && result->accepted[0].right_index == 0,
        "accepted matches must retain exported keypoint indices for exact parity checks");
  }
  const std::vector<int64_t> permuted_matches = {0, 2, 2, 0, 0, 0, 0, 1, 1};
  const std::vector<float> permuted_scores = {0.7f, 0.9f, 0.8f};
  auto permuted = hm::stitching::FeatureMatcher::Postprocess(
      metadata,
      keypoints.data(),
      keypoints.size(),
      permuted_matches.data(),
      permuted_matches.size(),
      permuted_scores.data(),
      permuted_scores.size(),
      5);
  ok &= expect(
      permuted.ok() && result.ok() && permuted->selected.size() == result->selected.size(),
      "permuted model rows must remain usable");
  if (permuted.ok() && result.ok()) {
    for (size_t index = 0; index < result->selected.size(); ++index) {
      ok &= expect(
          permuted->selected[index].left_index == result->selected[index].left_index &&
              permuted->selected[index].right_index == result->selected[index].right_index,
          "selected control points must be invariant to model row order");
    }
  }
  scores[1] = 0.2f;
  auto thresholded = hm::stitching::FeatureMatcher::Postprocess(
      metadata, keypoints.data(), keypoints.size(), matches.data(), matches.size(), scores.data(), scores.size(), 2);
  ok &= expect(thresholded.ok() && thresholded->accepted_match_count == 2, "score threshold must be strict > 0.2");

  matches[0] = 1;
  ok &= expect(
      !hm::stitching::FeatureMatcher::Postprocess(
           metadata,
           keypoints.data(),
           keypoints.size(),
           matches.data(),
           matches.size(),
           scores.data(),
           scores.size(),
           2)
           .ok(),
      "invalid pair index must fail");
  return ok ? 0 : 1;
}
