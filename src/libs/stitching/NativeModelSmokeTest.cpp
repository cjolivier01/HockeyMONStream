#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <random>
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
      model_path("HM_FEATURE_MATCHER_ONNX_MODEL", "superpoint-lightglue-pipeline-228994cea8c01014.onnx");
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
  auto matches = (*matcher)->Infer(left, right, 32);
  if (!matches.ok() || matches->accepted_match_count < 8 || matches->selected.size() != 32) {
    std::cerr << "FAIL: matcher model inference: " << (matches.ok() ? "too few matches" : matches.status().ToString())
              << '\n';
    return 1;
  }
  return 0;
}
