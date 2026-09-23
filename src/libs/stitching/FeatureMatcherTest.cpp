#include "hstream/src/libs/stitching/FeatureMatcher.h"

#include <algorithm>
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

bool test_spatial_selection() {
  using hm::stitching::FeatureMatch;
  using hm::stitching::FeatureMatcher;
  std::vector<FeatureMatch> accepted;
  // The wall spans many columns, while equally plentiful near-side matches
  // occupy one column per band. Density and confidence must not erase coverage.
  for (int row : {0, 1, 2, 3, 5, 6, 7, 8}) {
    for (int point = 0; point < 100; ++point) {
      const bool upper = row < 4;
      const cv::Point2f left(upper ? 50 + (point % 16) * 100 : 850, row * 100 + 10 + point * 0.5f);
      const int index = static_cast<int>(accepted.size());
      accepted.push_back({left, left + cv::Point2f(5, 3), upper ? 0.9f : 0.5f, index, index});
    }
  }
  const auto lower_count = [](const auto& matches) {
    return std::count_if(matches.begin(), matches.end(), [](const auto& match) { return match.left.y >= 450; });
  };
  bool ok = true;
  auto selected = FeatureMatcher::SelectControlPoints(accepted, {1600, 900}, 80);
  ok &= expect(
      selected.ok() && selected->size() == 80 && lower_count(*selected) == 40,
      "Wide upper clusters must not consume the budget of populated lower height bands");
  auto small = FeatureMatcher::SelectControlPoints(accepted, {1600, 900}, 4);
  ok &= expect(
      small.ok() && small->size() == 4 && lower_count(*small) == 2 && small->front().left.y < 100 &&
          small->back().left.y >= 800,
      "A cap smaller than the occupied band count must cover opposite image edges");
  std::reverse(accepted.begin(), accepted.end());
  auto reversed = FeatureMatcher::SelectControlPoints(accepted, {1600, 900}, 80);
  ok &= expect(
      selected.ok() && reversed.ok() && selected->size() == reversed->size() &&
          std::equal(
              selected->begin(),
              selected->end(),
              reversed->begin(),
              [](const auto& a, const auto& b) {
                return a.left_index == b.left_index && a.right_index == b.right_index;
              }),
      "Capped spatial selection must be invariant to input order");
  accepted.erase(
      std::remove_if(
          accepted.begin(),
          accepted.end(),
          [](const auto& match) { return match.left.y >= 450 && match.left_index % 100 >= 2; }),
      accepted.end());
  auto sparse = FeatureMatcher::SelectControlPoints(accepted, {1600, 900}, 80);
  ok &= expect(
      sparse.ok() && sparse->size() == 80 && lower_count(*sparse) == 8,
      "Exhausted sparse bands must give their remaining budget to real available matches");
  auto all = FeatureMatcher::SelectControlPoints(accepted, {1600, 900}, 1500);
  ok &= expect(
      all.ok() && all->size() == accepted.size() && lower_count(*all) == 8,
      "An inactive cap must preserve all accepted matches without inventing near-side points");
  return ok;
}

} // namespace

int main() {
  bool ok = test_spatial_selection();
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

  using hm::stitching::ControlPointResolution;
  const auto reference_resolution = hm::stitching::ParseControlPointResolution("1k");
  ok &= expect(
      hm::stitching::ParseControlPointResolution("native").ok() &&
          hm::stitching::ParseControlPointResolution("2k").ok() && reference_resolution.ok() &&
          *reference_resolution == ControlPointResolution::k1K &&
          std::string(hm::stitching::ControlPointResolutionName(ControlPointResolution::k1K)) == "1k" &&
          !hm::stitching::ParseControlPointResolution("bad-size").ok(),
      "resolution accepts native, 1k and 2k, rejecting unknown values");
  const auto auto_resolution = hm::stitching::ParseControlPointResolution("auto");
#ifdef IS_TEGRA
  const auto expected_default = ControlPointResolution::k2K;
#else
  const auto expected_default = ControlPointResolution::kNative;
#endif
  auto default_input = hm::stitching::FeatureMatcher::PrepareSuperPoint(left, right);
  ok &= expect(
      auto_resolution.ok() && *auto_resolution == expected_default && default_input.ok() &&
          default_input->tensor_size ==
              (expected_default == ControlPointResolution::k2K ? cv::Size(2048, 1152) : cv::Size(160, 104)),
      "auto and default preprocessing must use 2K on Jetson and native elsewhere");
  auto reduced = hm::stitching::FeatureMatcher::PrepareSuperPoint(left, right, ControlPointResolution::k2K);
  ok &= expect(
      reduced.ok() && reduced->tensor_size == cv::Size(2048, 1152) &&
          reduced->resized_sizes[0] == cv::Size(2048, 1152) && reduced->resized_sizes[1] == cv::Size(1152, 1152) &&
          reduced->tensor[static_cast<size_t>(2048) * 1152 + 1152] == 0.0f,
      "2K reproduces the aspect-preserving canvas and per-camera padding");
  auto reference = hm::stitching::FeatureMatcher::PrepareSuperPoint(left, right, ControlPointResolution::k1K);
  ok &= expect(
      reference.ok() && reference->tensor_size == cv::Size(1024, 1024) &&
          reference->resized_sizes[0] == cv::Size(1024, 576) && reference->resized_sizes[1] == cv::Size(1024, 1024) &&
          reference->tensor[576 * 1024] == 0.0f,
      "1K resizes each camera's long edge independently and pads their shared canvas");
  const cv::Mat portrait(301, 201, CV_8UC3, cv::Scalar::all(255));
  auto reference_portrait =
      hm::stitching::FeatureMatcher::PrepareSuperPoint(portrait, portrait, ControlPointResolution::k1K);
  ok &= expect(
      reference_portrait.ok() && reference_portrait->resized_sizes[0] == cv::Size(683, 1024) &&
          reference_portrait->tensor_size == cv::Size(688, 1024) &&
          std::abs(reference_portrait->tensor[682] - 1.0f) < 1e-6f && reference_portrait->tensor[683] == 0.0f,
      "1K preserves portrait aspect ratio and rounds only the padded canvas to multiples of eight");
  cv::Mat reference_pattern(256, 4096, CV_8UC3);
  for (int y = 0; y < reference_pattern.rows; ++y) {
    for (int x = 0; x < reference_pattern.cols; ++x)
      reference_pattern.at<cv::Vec3b>(y, x) = {
          static_cast<uchar>((x * 17 + y * 13) % 256),
          static_cast<uchar>((x * x + 7 * y) % 256),
          static_cast<uchar>((3 * x + 19 * y) % 256)};
  }
  auto antialiased = hm::stitching::FeatureMatcher::PrepareSuperPoint(
      reference_pattern, reference_pattern, ControlPointResolution::k1K);
  // Independent Kornia oracle: resize(rgb.float()/255, 1024, side="long",
  // antialias=True), then rgb_to_grayscale. Edge samples verify reflect padding.
  const struct {
    int y, x;
    float value;
  } reference_samples[] = {
      {0, 0, 0.105538331f},
      {0, 1, 0.212004855f},
      {0, 500, 0.523470461f},
      {1, 1023, 0.254430801f},
      {32, 500, 0.445597708f},
      {63, 0, 0.823443770f},
      {63, 1023, 0.759036183f}};
  ok &= expect(antialiased.ok(), "1K reference antialias fixture must preprocess");
  if (antialiased.ok()) {
    for (const auto& sample : reference_samples)
      ok &= expect(
          std::abs(antialiased->tensor[sample.y * antialiased->tensor_size.width + sample.x] - sample.value) < 2e-6f,
          "1K floating-point antialias preprocessing must match the independent Kornia oracle");
  }
  ok &= expect(
      !hm::stitching::FeatureMatcher::PrepareSuperPoint({}, right, ControlPointResolution::k1K).ok(),
      "1K rejects empty images before deriving their long-edge scale");
  auto superpoint = hm::stitching::FeatureMatcher::PrepareSuperPoint(left, right, ControlPointResolution::kNative);
  ok &= expect(superpoint.ok(), "valid SuperPoint images must preprocess");
  if (superpoint.ok()) {
    ok &= expect(
        superpoint->tensor_size == cv::Size(160, 104) && superpoint->resized_sizes[0] == left.size() &&
            superpoint->resized_sizes[1] == right.size(),
        "SuperPoint must keep each original size and pad the shared canvas to multiples of eight");
    const size_t image_plane = static_cast<size_t>(104) * 160;
    ok &= expect(superpoint->tensor.size() == 2 * image_plane, "SuperPoint must receive two grayscale planes");
    const float left_gray = (0.299f * 10 + 0.587f * 20 + 0.114f * 30) / 255.0f;
    const float right_gray = (0.299f * 40 + 0.587f * 50 + 0.114f * 60) / 255.0f;
    ok &= expect(
        std::abs(superpoint->tensor[89 * 160 + 159] - left_gray) < 1e-6f &&
            std::abs(superpoint->tensor[image_plane + 99 * 160 + 99] - right_gray) < 1e-6f,
        "SuperPoint must normalize grayscale through the last original pixel of both images");
    ok &= expect(
        superpoint->tensor[90 * 160] == 0.0f && superpoint->tensor[image_plane + 100] == 0.0f &&
            superpoint->tensor.back() == 0.0f,
        "SuperPoint padding must remain zero");
  }
  auto superpoint16 =
      hm::stitching::FeatureMatcher::PrepareSuperPoint(left16, right16, ControlPointResolution::kNative);
  ok &= expect(superpoint16.ok(), "16-bit SuperPoint images must preprocess");
  if (superpoint16.ok()) {
    ok &= expect(
        std::abs(superpoint16->tensor[0] - (0.299f * 1000 + 0.587f * 2000 + 0.114f * 3000) / 65535.0f) < 1e-6f,
        "16-bit SuperPoint input must use normalized grayscale");
  }
  ok &= expect(!hm::stitching::FeatureMatcher::PrepareSuperPoint({}, right).ok(), "empty SuperPoint images must fail");
  const cv::Mat tiny(2, 3, CV_8UC3, cv::Scalar::all(255));
  auto tiny_superpoint = hm::stitching::FeatureMatcher::PrepareSuperPoint(tiny, tiny, ControlPointResolution::kNative);
  ok &= expect(
      tiny_superpoint.ok() && tiny_superpoint->tensor_size == cv::Size(48, 48) &&
          tiny_superpoint->resized_sizes[0] == tiny.size() && tiny_superpoint->tensor[3] == 0.0f,
      "tiny SuperPoint images must pad for top-2048 without upscaling");

  cv::Mat full_size(2161, 3841, CV_8UC3, cv::Scalar::all(0));
  full_size.at<cv::Vec3b>(2160, 3840) = {255, 255, 255};
  full_size.at<cv::Vec3b>(2159, 3839) = {255, 255, 255};
  const cv::Mat cropped = full_size(cv::Rect(0, 0, 3840, 2160));
  auto full_superpoint =
      hm::stitching::FeatureMatcher::PrepareSuperPoint(full_size, cropped, ControlPointResolution::kNative);
  ok &= expect(full_superpoint.ok(), "full-resolution non-contiguous SuperPoint images must preprocess");
  if (full_superpoint.ok()) {
    const size_t image_plane = static_cast<size_t>(2168) * 3848;
    ok &= expect(
        full_superpoint->tensor_size == cv::Size(3848, 2168) && full_superpoint->resized_sizes[0] == full_size.size() &&
            full_superpoint->resized_sizes[1] == cropped.size(),
        "full-resolution images must not be downscaled or stretched to match the other camera");
    ok &= expect(
        std::abs(full_superpoint->tensor[2160 * 3848 + 3840] - 1.0f) < 1e-6f &&
            std::abs(full_superpoint->tensor[image_plane + 2159 * 3848 + 3839] - 1.0f) < 1e-6f &&
            full_superpoint->tensor[2160 * 3848 + 3839] == 0.0f &&
            full_superpoint->tensor[image_plane + 2160 * 3848 + 3840] == 0.0f,
        "single-pixel details at original image edges must survive unchanged and not leak into padding");
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
  metadata.resized_sizes[0] = {2048, 1152};
  metadata.resized_sizes[1] = {2048, 1152};
  metadata.tensor_size = {2048, 1152};
  std::vector<float> keypoints(
      static_cast<size_t>(2) * hm::stitching::FeatureMatcher::kSuperPointKeypointsPerImage * 2, 0.0f);
  auto set_keypoint = [&](int image, int index, float x, float y) {
    const size_t offset =
        (static_cast<size_t>(image) * hm::stitching::FeatureMatcher::kSuperPointKeypointsPerImage + index) * 2;
    keypoints[offset] = x;
    keypoints[offset + 1] = y;
  };
  set_keypoint(0, 0, 10.0f, 30.0f);
  set_keypoint(0, 1, 20.0f, 10.0f);
  set_keypoint(0, 2, 1500.0f, 900.0f);
  set_keypoint(1, 0, 40.0f, 35.0f);
  set_keypoint(1, 1, 50.0f, 15.0f);
  set_keypoint(1, 2, 1530.0f, 905.0f);
  std::vector<int64_t> matches = {0, 0, 0, 0, 1, 1, 0, 2, 2};
  std::vector<float> scores = {0.9f, 0.8f, 0.7f};
  set_keypoint(0, 2047, 70.0f, 60.0f);
  set_keypoint(1, 2047, 80.0f, 65.0f);
  const std::vector<int64_t> high_index_match = {0, 2047, 2047};
  auto high_index = hm::stitching::FeatureMatcher::Postprocess(
      metadata, keypoints.data(), keypoints.size(), high_index_match.data(), 3, scores.data(), 1, 5);
  ok &= expect(
      high_index.ok() && high_index->accepted.size() == 1 && high_index->accepted[0].left_index == 2047 &&
          high_index->accepted[0].right_index == 2047,
      "SuperPoint must retain matches from the entire 2048-keypoint detector budget");
  ok &= expect(
      !hm::stitching::FeatureMatcher::Postprocess(
           metadata, keypoints.data(), keypoints.size() / 2, matches.data(), 3, scores.data(), 1, 5)
           .ok(),
      "SuperPoint must reject the obsolete 1024-keypoint output contract");
  if (reference.ok()) {
    set_keypoint(0, 2047, 639.5f, 319.5f);
    set_keypoint(1, 2047, 511.5f, 511.5f);
    auto restored = hm::stitching::FeatureMatcher::Postprocess(
        *reference, keypoints.data(), keypoints.size(), high_index_match.data(), 3, scores.data(), 1, 5);
    ok &= expect(
        restored.ok() && cv::norm(restored->accepted[0].left - cv::Point2f(99.5f, 49.5f)) < 1e-4 &&
            cv::norm(restored->accepted[0].right - cv::Point2f(49.5f, 49.5f)) < 1e-4,
        "1K matches restore each camera's source pixel-center coordinates");
  }
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
        std::abs(result->selected[0].left.x - 76.375f) < 1e-4f, "inverse resize must preserve half-pixel centers");
    ok &= expect(
        std::abs(result->selected.back().left.x - 5626.375f) < 1e-4f &&
            std::abs(result->selected.back().right.y - 3395.125f) < 1e-4f,
        "matches beyond the old canvas must map back to source coordinates");
    ok &= expect(
        result->accepted[0].left_index == 0 && result->accepted[0].right_index == 0,
        "accepted matches must retain exported keypoint indices for exact parity checks");
  }
  if (full_superpoint.ok()) {
    set_keypoint(0, 0, 3840.0f, 2160.0f);
    set_keypoint(1, 0, 3839.0f, 2159.0f);
    // This point is inside the shared canvas, but outside the smaller right image.
    set_keypoint(1, 1, 3840.0f, 2160.0f);
    auto native_matches = hm::stitching::FeatureMatcher::Postprocess(
        *full_superpoint, keypoints.data(), keypoints.size(), matches.data(), 6, scores.data(), 2, 5);
    ok &= expect(
        native_matches.ok() && native_matches->accepted.size() == 1 &&
            native_matches->accepted[0].left == cv::Point2f(3840, 2160) &&
            native_matches->accepted[0].right == cv::Point2f(3839, 2159),
        "native-resolution matches must keep exact source coordinates and reject per-image padding");
    set_keypoint(0, 0, 10.0f, 30.0f);
    set_keypoint(1, 0, 40.0f, 35.0f);
    set_keypoint(1, 1, 50.0f, 15.0f);
  }
  if (reduced.ok()) {
    set_keypoint(0, 0, 1279.5f, 639.5f);
    set_keypoint(1, 0, 575.5f, 575.5f);
    set_keypoint(1, 1, 1200.0f, 500.0f);
    auto scaled = hm::stitching::FeatureMatcher::Postprocess(
        *reduced, keypoints.data(), keypoints.size(), matches.data(), 6, scores.data(), 2, 5);
    ok &= expect(
        scaled.ok() && scaled->accepted.size() == 1 &&
            cv::norm(scaled->accepted[0].left - cv::Point2f(99.5f, 49.5f)) < 1e-4 &&
            cv::norm(scaled->accepted[0].right - cv::Point2f(49.5f, 49.5f)) < 1e-4,
        "2K matches return source pixel-center coordinates and reject camera padding");
    set_keypoint(0, 0, 10.0f, 30.0f);
    set_keypoint(1, 0, 40.0f, 35.0f);
    set_keypoint(1, 1, 50.0f, 15.0f);
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
  cv::Mat akaze_scene(360, 960, CV_8UC3, cv::Scalar::all(12));
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> x_distribution(30, akaze_scene.cols - 40);
  std::uniform_int_distribution<int> y_distribution(30, akaze_scene.rows - 40);
  std::uniform_int_distribution<int> color_distribution(40, 255);
  for (int marker = 0; marker < 240; ++marker) {
    const cv::Point center(x_distribution(rng), y_distribution(rng));
    const cv::Scalar color(color_distribution(rng), color_distribution(rng), color_distribution(rng));
    cv::circle(akaze_scene, center, 3 + marker % 7, color, cv::FILLED);
    cv::line(akaze_scene, center - cv::Point(8, 0), center + cv::Point(8, 0), cv::Scalar::all(255), 1);
  }
  const cv::Mat akaze_left = akaze_scene(cv::Rect(0, 0, 640, 360)).clone();
  const cv::Mat akaze_right = akaze_scene(cv::Rect(320, 0, 640, 360)).clone();
  if (akaze.ok()) {
    auto akaze_result = (*akaze)->Infer(akaze_left, akaze_right, 64);
    ok &= expect(
        akaze_result.ok() && akaze_result->accepted_match_count >= 6,
        "AKAZE M-LDB/Hamming must match the expected overlap between cropped views");
    if (akaze_result.ok()) {
      size_t translated = 0;
      for (const auto& match : akaze_result->accepted) {
        ok &= expect(
            match.left.x >= 0.5f * akaze_left.cols && match.right.x <= 0.5f * akaze_right.cols &&
                match.left.y >= 0.2f * akaze_left.rows && match.left.y <= 0.8f * akaze_left.rows &&
                match.right.y >= 0.2f * akaze_right.rows && match.right.y <= 0.8f * akaze_right.rows,
            "AKAZE matches must remain inside the expected overlap and vertical band");
        if (std::abs((match.right.x - match.left.x) + 320.0f) < 1.0f && std::abs(match.right.y - match.left.y) < 1.0f) {
          ++translated;
        }
      }
      ok &= expect(
          translated * 4 >= akaze_result->accepted.size() * 3,
          "most AKAZE epipolar inliers must recover the synthetic crop offset");
    }
  }

  hm::stitching::FisheyeLensCalibration lens;
  lens.resolution = akaze_left.size();
  lens.fx = 500.0;
  lens.fy = 500.0;
  lens.cx = 320.0;
  lens.cy = 180.0;
  lens.distortion = {1e-4, -1e-6, 1e-8, -1e-10};
  auto calibrated_akaze = hm::stitching::FeatureMatcher::Create(
      "",
      hm::stitching::ControlPointMatcher::kAkazeHamming,
      hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
  ok &= expect(calibrated_akaze.ok(), "AKAZE must accept valid paired KB4 lens calibration");
  if (calibrated_akaze.ok()) {
    auto calibrated_result = (*calibrated_akaze)->Infer(akaze_left, akaze_right, 64);
    ok &= expect(
        calibrated_result.ok() && calibrated_result->accepted_match_count >= 6,
        "calibrated AKAZE must undistort detection and return geometrically filtered matches");
    if (calibrated_result.ok()) {
      for (const auto& match : calibrated_result->accepted) {
        ok &= expect(
            match.left.x >= 0.0f && match.left.x < akaze_left.cols && match.left.y >= 0.0f &&
                match.left.y < akaze_left.rows && match.right.x >= 0.0f && match.right.x < akaze_right.cols &&
                match.right.y >= 0.0f && match.right.y < akaze_right.rows,
            "calibrated AKAZE must return rectified keypoints inside the source-sized mapping domain");
      }
    }
  }
  auto incomplete_calibration = hm::stitching::FeatureMatcher::Create(
      "", hm::stitching::ControlPointMatcher::kAkazeHamming, hm::stitching::AkazeMatchingCalibration{.left = lens});
  ok &= expect(!incomplete_calibration.ok(), "AKAZE must reject calibration for only one camera");
  return ok ? 0 : 1;
}
