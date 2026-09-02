#include "hstream/src/libs/stitching/FeatureMatcher.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

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
      hm::stitching::ParseControlPointMatcher("dedode-lightglue").ok(), "HockeyMOM DeDoDe matcher must be accepted");
  ok &= expect(hm::stitching::ParseControlPointMatcher("loftr").ok(), "HockeyMOM LoFTR matcher must be accepted");
  auto akaze_choice = hm::stitching::ParseControlPointMatcher("akaze-mldb-hamming");
  ok &= expect(
      akaze_choice.ok() && hm::stitching::ControlPointMatcherName(*akaze_choice) == std::string("akaze-hamming"),
      "AKAZE M-LDB/Hamming spelling must canonicalize");
  auto dedode = hm::stitching::ParseControlPointMatcher("dedode-lightglue");
  ok &= expect(
      dedode.ok() && !hm::stitching::FeatureMatcher::Create("/tmp/missing.onnx", *dedode).ok(),
      "unsupported matcher engines must fail explicitly when instantiated");
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
  cv::Mat left16(90, 160, CV_16UC3, cv::Scalar(3000, 2000, 1000));
  cv::Mat right16(100, 100, CV_16UC3, cv::Scalar(6000, 5000, 4000));
  auto prepared16 = hm::stitching::FeatureMatcher::Prepare(left16, right16);
  ok &= expect(prepared16.ok(), "16-bit feature images must preprocess");
  if (prepared16.ok()) {
    ok &= expect(std::abs(prepared16->tensor[0] - 1000.0f / 65535.0f) < 1e-6f, "16-bit BGR must become RGB in [0,1]");
    const size_t second_image = static_cast<size_t>(3) * 576 * 1024;
    ok &= expect(
        std::abs(prepared16->tensor[second_image] - 4000.0f / 65535.0f) < 1e-6f,
        "second 16-bit image must use the same preprocessing");
  }

  auto loftr_prepared = hm::stitching::FeatureMatcher::PrepareLoFTR(left, right);
  ok &= expect(loftr_prepared.ok(), "valid LoFTR images must preprocess");
  if (loftr_prepared.ok()) {
    ok &= expect(
        loftr_prepared->resized_sizes[0] == cv::Size(160, 64) && loftr_prepared->resized_sizes[1] == cv::Size(96, 96) &&
            loftr_prepared->tensor_size == cv::Size(160, 96),
        "LoFTR inputs must align down to multiples of 32 and share a padded tensor size");
    ok &= expect(
        loftr_prepared->tensor.size() == static_cast<size_t>(2) * 160 * 96,
        "LoFTR tensor must contain two shared-size grayscale planes");
    ok &= expect(
        std::abs(loftr_prepared->tensor[0] - 18.0f / 255.0f) < 1e-6f,
        "LoFTR preprocessing must convert BGR to normalized grayscale");
    ok &= expect(
        loftr_prepared->tensor[static_cast<size_t>(64) * 160] == 0.0f, "LoFTR shared-size padding must remain zero");
  }

  hm::stitching::FeaturePairInput metadata;
  metadata.source_sizes[0] = {7680, 4320};
  metadata.source_sizes[1] = {7680, 4320};
  metadata.resized_sizes[0] = {1024, 576};
  metadata.resized_sizes[1] = {1024, 576};
  std::vector<float> keypoints(static_cast<size_t>(2) * hm::stitching::FeatureMatcher::kKeypointsPerImage * 2, 0.0f);
  auto set_keypoint = [&](int image, int index, float x, float y) {
    const size_t offset = (static_cast<size_t>(image) * hm::stitching::FeatureMatcher::kKeypointsPerImage + index) * 2;
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

  hm::stitching::FeaturePairInput dedode_metadata;
  dedode_metadata.source_sizes[0] = {2048, 1152};
  dedode_metadata.source_sizes[1] = {2048, 1152};
  dedode_metadata.resized_sizes[0] = {1024, 576};
  dedode_metadata.resized_sizes[1] = {1024, 576};
  dedode_metadata.tensor_size = {1024, 576};
  std::vector<float> dedode_keypoints(
      static_cast<size_t>(2) * hm::stitching::FeatureMatcher::kKeypointsPerImage * 2, 0.0f);
  std::vector<int64_t> dedode_matches(hm::stitching::FeatureMatcher::kKeypointsPerImage, -1);
  std::vector<float> dedode_scores(hm::stitching::FeatureMatcher::kKeypointsPerImage, 0.0f);
  dedode_keypoints[0] = 100.0f;
  dedode_keypoints[1] = 80.0f;
  const size_t dedode_right = static_cast<size_t>(hm::stitching::FeatureMatcher::kKeypointsPerImage) * 2;
  dedode_keypoints[dedode_right] = 104.0f;
  dedode_keypoints[dedode_right + 1] = 82.0f;
  dedode_matches[0] = 0;
  dedode_scores[0] = 0.9f;
  auto dedode_result = hm::stitching::FeatureMatcher::PostprocessDeDoDe(
      dedode_metadata,
      dedode_keypoints.data(),
      dedode_keypoints.size(),
      dedode_matches.data(),
      dedode_matches.size(),
      dedode_scores.data(),
      dedode_scores.size(),
      8);
  ok &= expect(
      dedode_result.ok() && dedode_result->accepted.size() == 1 &&
          std::abs(dedode_result->accepted[0].left.x - 200.0f) < 1e-6f,
      "DeDoDe indexed matches must scale back to source coordinates");

  hm::stitching::FeaturePairInput loftr_metadata;
  loftr_metadata.source_sizes[0] = {3200, 1800};
  loftr_metadata.source_sizes[1] = {1600, 900};
  loftr_metadata.resized_sizes[0] = {1600, 896};
  loftr_metadata.resized_sizes[1] = {1600, 896};
  loftr_metadata.tensor_size = {1600, 896};
  const float loftr_left[] = {800.0f, 448.0f, 100.0f, 100.0f};
  const float loftr_right[] = {810.0f, 450.0f, 110.0f, 102.0f};
  const float loftr_scores[] = {0.8f, 0.2f};
  auto loftr_result = hm::stitching::FeatureMatcher::PostprocessLoFTR(
      loftr_metadata, loftr_left, 4, loftr_right, 4, loftr_scores, 2, 8);
  ok &= expect(
      loftr_result.ok() && loftr_result->accepted.size() == 1 &&
          std::abs(loftr_result->accepted[0].left.x - 1600.0f) < 1e-6f,
      "LoFTR matches must apply the strict score threshold and source-coordinate scale");

  auto akaze = hm::stitching::FeatureMatcher::Create("", hm::stitching::ControlPointMatcher::kAkazeHamming);
  ok &= expect(akaze.ok(), "AKAZE must not require an ONNX model");
  cv::Mat akaze_left(360, 640, CV_8UC3, cv::Scalar::all(12));
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> x_distribution(30, akaze_left.cols - 40);
  std::uniform_int_distribution<int> y_distribution(30, akaze_left.rows - 40);
  std::uniform_int_distribution<int> color_distribution(40, 255);
  for (int marker = 0; marker < 160; ++marker) {
    const cv::Point center(x_distribution(rng), y_distribution(rng));
    const cv::Scalar color(color_distribution(rng), color_distribution(rng), color_distribution(rng));
    cv::circle(akaze_left, center, 3 + marker % 7, color, cv::FILLED);
    cv::line(akaze_left, center - cv::Point(8, 0), center + cv::Point(8, 0), cv::Scalar::all(255), 1);
  }
  cv::Mat akaze_right;
  const cv::Mat translation = (cv::Mat_<double>(2, 3) << 1, 0, 9, 0, 1, 4);
  cv::warpAffine(akaze_left, akaze_right, translation, akaze_left.size(), cv::INTER_LINEAR, cv::BORDER_CONSTANT);
  if (akaze.ok()) {
    auto akaze_result = (*akaze)->Infer(akaze_left, akaze_right, 64);
    ok &= expect(
        akaze_result.ok() && akaze_result->accepted_match_count >= 16,
        "AKAZE M-LDB/Hamming must match a translated textured image");
    if (akaze_result.ok()) {
      size_t translated = 0;
      for (const auto& match : akaze_result->accepted) {
        if (std::abs((match.right.x - match.left.x) - 9.0f) < 1.0f &&
            std::abs((match.right.y - match.left.y) - 4.0f) < 1.0f) {
          ++translated;
        }
      }
      ok &= expect(
          translated * 4 >= akaze_result->accepted.size() * 3,
          "most AKAZE mutual matches must recover the synthetic translation");
    }
  }
  return ok ? 0 : 1;
}
