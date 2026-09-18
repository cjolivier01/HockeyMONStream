#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

namespace fs = std::filesystem;

namespace {

bool required() {
  const char* value = std::getenv("HM_REQUIRE_ONNX_MODEL_TESTS");
  return value != nullptr && std::string(value) == "1";
}

fs::path model_path(const char* variable, const char* filename) {
  if (const char* value = std::getenv(variable); value != nullptr && *value != '\0')
    return value;
  const char* home = std::getenv("HOME");
  return fs::path(home == nullptr ? "/nonexistent" : home) / ".cache/hstream/models" / filename;
}

bool fail_or_skip(const std::string& message) {
  if (required()) {
    std::cerr << "FAIL: " << message << '\n';
    return false;
  }
  std::cout << "SKIP: " << message << '\n';
  return true;
}

} // namespace

int main() {
  const char* requested_matcher = std::getenv("HM_MATCHER_SMOKE_NAME");
  std::string only_matcher;
  if (requested_matcher && *requested_matcher) {
    const auto parsed_matcher = hm::stitching::ParseControlPointMatcher(requested_matcher);
    if (!parsed_matcher.ok()) {
      std::cerr << "FAIL: unknown requested matcher\n";
      return 1;
    }
    only_matcher = hm::stitching::ControlPointMatcherName(*parsed_matcher);
  }
  const char* requested_provider = std::getenv("HM_MATCHER_SMOKE_PROVIDER");
  auto provider = hm::onnx::ParseExecutionProvider(requested_provider ? requested_provider : "cpu");
  if (!provider.ok()) {
    std::cerr << provider.status() << '\n';
    return 1;
  }
  const char* require_fallback = std::getenv("HM_REQUIRE_CPU_FALLBACK");
  const bool require_cpu_fallback = require_fallback && std::string(require_fallback) == "1";
  if (require_cpu_fallback &&
      (*provider != hm::onnx::ExecutionProvider::kCuda || only_matcher.empty() || only_matcher == "akaze-hamming")) {
    std::cerr << "FAIL: CPU fallback smoke requires CUDA and one selected neural matcher\n";
    return 1;
  }
  const fs::path rink_path = model_path("HM_RINK_ONNX_MODEL", "ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx");
  const fs::path matcher_path = model_path(
      "HM_SUPERPOINT_LIGHTGLUE_ONNX_MODEL",
      *provider == hm::onnx::ExecutionProvider::kCuda ? "superpoint-lightglue-cuda-0f3d76a65c832fc1.onnx"
                                                      : "superpoint-lightglue-pipeline-228994cea8c01014.onnx");
  const fs::path legacy_aliked_path =
      model_path("HM_FEATURE_MATCHER_ONNX_MODEL", "aliked-lightglue-k2048-ea4a4ab2cb556958.onnx");
  const fs::path dedode_path =
      model_path("HM_DEDODE_LIGHTGLUE_ONNX_MODEL", "dedode-lightglue-lc4v2-bupright-f8bd053e44d57a77.onnx");
  const fs::path loftr_path = model_path("HM_LOFTR_ONNX_MODEL", "efficient-loftr-outdoor-opt-a2cbdcfef0ddb5cd.onnx");
  auto needs = [&](const std::string& name) { return only_matcher.empty() || only_matcher == name; };
  if (!fs::is_regular_file(rink_path) || (needs("superpoint-lightglue") && !fs::is_regular_file(matcher_path)) ||
      (only_matcher.empty() && !fs::is_regular_file(legacy_aliked_path)) ||
      (needs("dedode-lightglue") && !fs::is_regular_file(dedode_path)) ||
      (needs("loftr") && !fs::is_regular_file(loftr_path))) {
    return fail_or_skip("native calibration model assets are not cached") ? 0 : 1;
  }

  auto rink = hm::stitching::RinkSegmentation::Create(rink_path.string(), {}, *provider);
  if (!rink.ok()) {
    std::cerr << "FAIL: rink model contract: " << rink.status() << '\n';
    return 1;
  }
  cv::Mat synthetic_rink(360, 640, CV_8UC3, cv::Scalar(35, 35, 35));
  cv::rectangle(synthetic_rink, cv::Rect(40, 50, 560, 280), cv::Scalar(225, 225, 225), cv::FILLED);
  auto rink_result = (*rink)->Infer(synthetic_rink);
  if (!rink_result.ok() && rink_result.status().code() != absl::StatusCode::kNotFound) {
    std::cerr << "FAIL: rink model inference: " << rink_result.status() << '\n';
    return 1;
  }
  rink->reset();

  if (const char* rink_image = std::getenv("HM_RINK_SMOKE_IMAGE")) {
    const cv::Mat frame = cv::imread(rink_image, cv::IMREAD_COLOR);
    auto cuda_rink = hm::stitching::RinkSegmentation::Create(rink_path.string(), {}, *provider);
    if (frame.empty() || !cuda_rink.ok())
      return 1;
    auto actual = (*cuda_rink)->Infer(frame, hm::stitching::RinkSegmentation::kHockeyMomInferenceScale);
    cuda_rink->reset();
    auto cpu_rink = hm::stitching::RinkSegmentation::Create(rink_path.string(), {}, hm::onnx::ExecutionProvider::kCpu);
    if (!actual.ok() || !cpu_rink.ok())
      return 1;
    auto expected = (*cpu_rink)->Infer(frame, hm::stitching::RinkSegmentation::kHockeyMomInferenceScale);
    if (!expected.ok())
      return 1;
    cv::Mat intersection, united;
    cv::bitwise_and(actual->combined_mask, expected->combined_mask, intersection);
    cv::bitwise_or(actual->combined_mask, expected->combined_mask, united);
    const double iou = static_cast<double>(cv::countNonZero(intersection)) / std::max(1, cv::countNonZero(united));
    std::cout << "Real rink mask CPU/provider IoU=" << iou << '\n';
    if (iou < 0.99)
      return 1;
  }

  cv::Mat texture(576, 1024, CV_8UC1);
  std::mt19937 rng(3);
  std::uniform_int_distribution<int> pixel_value(0, 255);
  for (int y = 0; y < texture.rows; ++y) {
    uchar* row = texture.ptr<uchar>(y);
    for (int x = 0; x < texture.cols; ++x) {
      row[x] = static_cast<uchar>(pixel_value(rng));
    }
  }
  cv::GaussianBlur(texture, texture, cv::Size(), 1.2);
  cv::Mat left;
  cv::cvtColor(texture, left, cv::COLOR_GRAY2BGR);
  std::uniform_int_distribution<int> x_distribution(30, left.cols - 31);
  std::uniform_int_distribution<int> y_distribution(30, left.rows - 31);
  std::uniform_int_distribution<int> radius_distribution(3, 8);
  for (int marker = 0; marker < 200; ++marker) {
    const cv::Point center(x_distribution(rng), y_distribution(rng));
    const int radius = radius_distribution(rng);
    const cv::Scalar color(pixel_value(rng), pixel_value(rng), pixel_value(rng));
    cv::circle(left, center, radius, color, cv::FILLED);
  }
  cv::Mat right;
  const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, 7, 0, 1, 3);
  cv::warpAffine(left, right, transform, left.size());
  if (only_matcher.empty()) {
    auto legacy_aliked = hm::stitching::FeatureMatcher::CreateLegacyAlikedParity(legacy_aliked_path.string());
    if (!legacy_aliked.ok()) {
      std::cerr << "FAIL: legacy RaCo-ALIKED parity model contract: " << legacy_aliked.status() << '\n';
      return 1;
    }
    auto legacy_matches = (*legacy_aliked)->Infer(left, right, 32);
    if (!legacy_matches.ok() || legacy_matches->accepted_match_count < 8 || legacy_matches->selected.empty() ||
        legacy_matches->selected.size() > 32) {
      std::cerr << "FAIL: legacy RaCo-ALIKED parity inference: "
                << (legacy_matches.ok() ? "too few matches" : legacy_matches.status().ToString()) << '\n';
      return 1;
    }
    legacy_aliked->reset();
  }
  struct MatcherCase {
    const char* name;
    hm::stitching::ControlPointMatcher matcher;
    fs::path path;
  };
  const MatcherCase cases[] = {
      {"SuperPoint + LightGlue", hm::stitching::ControlPointMatcher::kSuperPointLightGlue, matcher_path},
      {"DeDoDe + LightGlue", hm::stitching::ControlPointMatcher::kDeDoDeLightGlue, dedode_path},
      {"EfficientLoFTR outdoor", hm::stitching::ControlPointMatcher::kLoFTR, loftr_path},
      {"AKAZE + M-LDB + Hamming", hm::stitching::ControlPointMatcher::kAkazeHamming, {}},
  };
  const char* requested_resolution = std::getenv("HM_SUPERPOINT_SMOKE_RESOLUTION");
  auto resolution = hm::stitching::ParseControlPointResolution(requested_resolution ? requested_resolution : "auto");
  if (!resolution.ok()) {
    std::cerr << resolution.status() << '\n';
    return 1;
  }
  const char* profile_dir = std::getenv("HM_MATCHER_SMOKE_PROFILE_DIR");
  for (const auto& matcher_case : cases) {
    if (!needs(hm::stitching::ControlPointMatcherName(matcher_case.matcher)))
      continue;
    const std::string profile_prefix = profile_dir
        ? (fs::path(profile_dir) / hm::stitching::ControlPointMatcherName(matcher_case.matcher)).string()
        : "";
    int fallback_count = 0;
    hm::onnx::CpuFallbackOptions fallback;
    if (require_cpu_fallback) {
      fallback.model_path = matcher_case.matcher == hm::stitching::ControlPointMatcher::kSuperPointLightGlue
          ? model_path("HM_FEATURE_MATCHER_CPU_ONNX_MODEL", "superpoint-lightglue-pipeline-228994cea8c01014.onnx")
                .string()
          : matcher_case.path.string();
      fallback.on_fallback = [&] { ++fallback_count; };
    }
    auto matcher = hm::stitching::FeatureMatcher::Create(
        matcher_case.path.string(), matcher_case.matcher, {}, *resolution, *provider, profile_prefix, fallback);
    if (!matcher.ok()) {
      std::cerr << "FAIL: " << matcher_case.name << " model contract: " << matcher.status() << '\n';
      return 1;
    }
    // AKAZE expects the right half of the left camera to overlap the left half of the right camera.
    const bool akaze = matcher_case.matcher == hm::stitching::ControlPointMatcher::kAkazeHamming;
    cv::Mat matcher_left = akaze ? left(cv::Rect(0, 0, 640, left.rows)).clone() : left;
    cv::Mat matcher_right = akaze ? left(cv::Rect(320, 0, 640, left.rows)).clone() : right;
    const bool superpoint = matcher_case.matcher == hm::stitching::ControlPointMatcher::kSuperPointLightGlue;
    const char* game_dir = std::getenv("HM_SUPERPOINT_SMOKE_GAME_DIR");
    const bool real_superpoint_images = superpoint && game_dir != nullptr && *game_dir != '\0';
    if (real_superpoint_images) {
      constexpr int flags = cv::IMREAD_COLOR | cv::IMREAD_ANYDEPTH | cv::IMREAD_IGNORE_ORIENTATION;
      matcher_left = cv::imread((fs::path(game_dir) / "left.png").string(), flags);
      matcher_right = cv::imread((fs::path(game_dir) / "right.png").string(), flags);
      if (matcher_left.empty() || matcher_right.empty()) {
        std::cerr << "FAIL: SuperPoint game fixture must contain left.png and right.png\n";
        return 1;
      }
    } else if (superpoint) {
      // Exercise native 4K-sized input, stride alignment, and unequal camera dimensions.
      cv::resize(left, matcher_left, {3841, 2161});
      cv::warpAffine(matcher_left, matcher_right, transform, matcher_left.size());
      matcher_right = matcher_right(cv::Rect(0, 0, 3837, 2157));
    }
    const auto started = std::chrono::steady_clock::now();
    auto matches = (*matcher)->Infer(matcher_left, matcher_right, 32);
    if (!fallback.model_path.empty() && fallback_count != 1) {
      std::cerr << "FAIL: requested GPU OOM smoke must exercise CPU fallback exactly once\n";
      return 1;
    }
    if (!matches.ok() || matches->accepted_match_count < 8 || matches->selected.empty() ||
        matches->selected.size() > 32) {
      std::cerr << "FAIL: " << matcher_case.name
                << " inference: " << (matches.ok() ? "too few matches" : matches.status().ToString()) << '\n';
      return 1;
    }
    if (superpoint && !real_superpoint_images) {
      size_t translated = 0;
      for (const auto& match : matches->accepted) {
        if (cv::norm(match.right - match.left - cv::Point2f(7.0f, 3.0f)) < 2.0)
          ++translated;
      }
      if (translated * 2 < matches->accepted.size()) {
        std::cerr
            << "FAIL: SuperPoint matches must preserve the source translation after inference in source coordinates\n";
        return 1;
      }
    }
    std::cout << matcher_case.name
              << (superpoint ? std::string(" resolution=") + hm::stitching::ControlPointResolutionName(*resolution)
                             : "")
              << " (" << matcher_left.cols << 'x' << matcher_left.rows << ", " << matcher_right.cols << 'x'
              << matcher_right.rows << "): " << matches->accepted_match_count << " matches in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s\n";
  }
  return 0;
}
