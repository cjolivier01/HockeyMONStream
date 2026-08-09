#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

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
  const fs::path rink_path = model_path("HM_RINK_ONNX_MODEL", "ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx");
  const fs::path matcher_path =
      model_path("HM_FEATURE_MATCHER_ONNX_MODEL", "aliked-lightglue-k2048-ea4a4ab2cb556958.onnx");
  if (!fs::is_regular_file(rink_path) || !fs::is_regular_file(matcher_path)) {
    return fail_or_skip("native calibration model assets are not cached") ? 0 : 1;
  }

  auto rink = hm::stitching::RinkSegmentation::Create(rink_path.string());
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

  auto matcher = hm::stitching::FeatureMatcher::Create(matcher_path.string());
  if (!matcher.ok()) {
    std::cerr << "FAIL: matcher model contract: " << matcher.status() << '\n';
    return 1;
  }
  cv::Mat left(576, 1024, CV_8UC3, cv::Scalar(10, 10, 10));
  for (int y = 32; y < left.rows; y += 64) {
    for (int x = 32; x < left.cols; x += 64) {
      const cv::Scalar color((x + y) % 255, (2 * x + y) % 255, (x + 2 * y) % 255);
      cv::circle(left, cv::Point(x, y), 9, color, cv::FILLED);
      cv::line(left, cv::Point(x - 12, y - 12), cv::Point(x + 12, y + 12), color, 2);
    }
  }
  cv::Mat right;
  const cv::Mat transform = (cv::Mat_<double>(2, 3) << 1, 0, 7, 0, 1, 3);
  cv::warpAffine(left, right, transform, left.size());
  auto matches = (*matcher)->Infer(left, right, 32);
  if (!matches.ok() || matches->accepted_match_count < 8 || matches->selected.size() != 32) {
    std::cerr << "FAIL: matcher model inference: " << (matches.ok() ? "too few matches" : matches.status().ToString())
              << '\n';
    return 1;
  }
  return 0;
}
