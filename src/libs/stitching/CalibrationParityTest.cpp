#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/Orientation.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include <spawn.h>
#include <sys/wait.h>

#include <opencv2/calib3d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <yaml-cpp/yaml.h>

extern char** environ;
namespace fs = std::filesystem;

namespace {

bool required() {
  const char* value = std::getenv("HM_REQUIRE_ONNX_PARITY");
  return value != nullptr && std::string(value) == "1";
}

int skip_or_fail(const std::string& message) {
  std::cerr << (required() ? "FAIL: " : "SKIP: ") << message << '\n';
  return required() ? 1 : 0;
}

std::string python_executable() {
  for (const char* variable : {"HM_PARITY_PYTHON", "HM_PYTHON", "PYTHON_BIN"}) {
    if (const char* value = std::getenv(variable); value != nullptr && *value != '\0')
      return value;
  }
  return "python3";
}

int run_reference(const std::vector<std::string>& arguments) {
  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (const std::string& argument : arguments)
    argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  pid_t child = -1;
  const int spawn_status = ::posix_spawnp(&child, argv[0], nullptr, nullptr, argv.data(), environ);
  if (spawn_status != 0)
    return 127;
  int status = 0;
  if (::waitpid(child, &status, 0) < 0 || !WIFEXITED(status))
    return 127;
  return WEXITSTATUS(status);
}

fs::path model_path(const char* variable, const char* filename) {
  if (const char* value = std::getenv(variable); value != nullptr && *value != '\0')
    return value;
  const char* home = std::getenv("HOME");
  return fs::path(home == nullptr ? "/nonexistent" : home) / ".cache/hmstream/models" / filename;
}

std::vector<cv::Point2f> yaml_points(const YAML::Node& node) {
  std::vector<cv::Point2f> points;
  if (!node || !node.IsSequence())
    return points;
  for (const auto& point : node) {
    if (point.IsSequence() && point.size() == 2)
      points.emplace_back(point[0].as<float>(), point[1].as<float>());
  }
  return points;
}

cv::Mat homography(const std::vector<cv::Point2f>& left, const std::vector<cv::Point2f>& right) {
  if (left.size() < 8 || left.size() != right.size())
    return {};
  return cv::findHomography(left, right, cv::RANSAC, 3.0);
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2)
    return skip_or_fail("Python parity helper was not provided");
  const char* home = std::getenv("HOME");
  const fs::path game_dir = std::getenv("HM_ONNX_PARITY_GAME_DIR") != nullptr
      ? fs::path(std::getenv("HM_ONNX_PARITY_GAME_DIR"))
      : fs::path(home == nullptr ? "/nonexistent" : home) / "Videos/tv-12-1-r2";
  const fs::path rink_model = model_path("HM_RINK_ONNX_MODEL", "ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx");
  const fs::path matcher_model =
      model_path("HM_FEATURE_MATCHER_ONNX_MODEL", "aliked-lightglue-k2048-ea4a4ab2cb556958.onnx");
  if (!fs::is_regular_file(game_dir / "s.png") || !fs::is_regular_file(game_dir / "left.png") ||
      !fs::is_regular_file(game_dir / "right.png") || !fs::is_regular_file(rink_model) ||
      !fs::is_regular_file(matcher_model)) {
    return skip_or_fail("native models and calibrated game fixtures are required for Python parity");
  }

  char temporary_template[] = "/tmp/hmstream-python-parity-XXXXXX";
  const char* temporary = ::mkdtemp(temporary_template);
  if (temporary == nullptr)
    return 1;
  const fs::path output_dir(temporary);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{output_dir};
  const int reference_status = run_reference({
      python_executable(),
      argv[1],
      "--game-dir",
      game_dir.string(),
      "--output-dir",
      output_dir.string(),
      "--max-control-points",
      "128",
  });
  if (reference_status == 77 || reference_status == 127) {
    return skip_or_fail("HockeyMOM/MMDetection Python reference is not available");
  }
  if (reference_status != 0) {
    std::cerr << "FAIL: Python parity reference exited " << reference_status << '\n';
    return 1;
  }

  const cv::Mat stitched = cv::imread((game_dir / "s.png").string(), cv::IMREAD_COLOR);
  const cv::Mat left = cv::imread((game_dir / "left.png").string(), cv::IMREAD_COLOR);
  const cv::Mat right = cv::imread((game_dir / "right.png").string(), cv::IMREAD_COLOR);
  auto rink = hm::stitching::RinkSegmentation::Create(rink_model.string());
  auto matcher = hm::stitching::FeatureMatcher::Create(matcher_model.string());
  if (!rink.ok() || !matcher.ok()) {
    std::cerr << "FAIL: native parity model contract failed\n";
    return 1;
  }
  auto native_rink = (*rink)->Infer(stitched);
  auto native_matches = (*matcher)->Infer(left, right, 128);
  if (!native_rink.ok() || !native_matches.ok()) {
    std::cerr << "FAIL: native parity inference failed: "
              << (native_rink.ok() ? native_matches.status().ToString() : native_rink.status().ToString()) << '\n';
    return 1;
  }

  const cv::Mat python_mask = cv::imread((output_dir / "python_rink_mask.png").string(), cv::IMREAD_GRAYSCALE);
  if (python_mask.empty() || python_mask.size() != native_rink->combined_mask.size())
    return 1;
  cv::Mat intersection;
  cv::Mat union_mask;
  cv::bitwise_and(python_mask, native_rink->combined_mask, intersection);
  cv::bitwise_or(python_mask, native_rink->combined_mask, union_mask);
  const double iou = static_cast<double>(cv::countNonZero(intersection)) / cv::countNonZero(union_mask);
  YAML::Node reference = YAML::LoadFile((output_dir / "python_reference.yaml").string());
  const double centroid_dx = std::abs(native_rink->centroid.x - reference["centroid"][0].as<double>());
  const double centroid_dy = std::abs(native_rink->centroid.y - reference["centroid"][1].as<double>());
  const std::vector<double> native_bbox = {
      native_rink->combined_bbox.x,
      native_rink->combined_bbox.y,
      native_rink->combined_bbox.x + native_rink->combined_bbox.width,
      native_rink->combined_bbox.y + native_rink->combined_bbox.height,
  };
  double maximum_bbox_delta = 0.0;
  for (size_t i = 0; i < native_bbox.size(); ++i) {
    maximum_bbox_delta = std::max(maximum_bbox_delta, std::abs(native_bbox[i] - reference["bbox"][i].as<double>()));
  }
  auto native_orientation = hm::stitching::classify_rink_orientation(native_rink->combined_mask);
  auto python_orientation = hm::stitching::classify_rink_orientation(python_mask);
  if (iou < 0.99 || centroid_dx > 2.0 || centroid_dy > 2.0 || maximum_bbox_delta > 2.0 || !native_orientation.ok() ||
      !python_orientation.ok() || *native_orientation != *python_orientation) {
    std::cerr << "FAIL: rink parity iou=" << iou << " centroid_delta=" << centroid_dx << ',' << centroid_dy
              << " bbox_delta=" << maximum_bbox_delta << '\n';
    return 1;
  }

  std::vector<cv::Point2f> native_left;
  std::vector<cv::Point2f> native_right;
  for (const auto& match : native_matches->selected) {
    native_left.push_back(match.left);
    native_right.push_back(match.right);
  }
  const std::vector<cv::Point2f> python_left = yaml_points(reference["left_points"]);
  const std::vector<cv::Point2f> python_right = yaml_points(reference["right_points"]);
  const cv::Mat native_h = homography(native_left, native_right);
  const cv::Mat python_h = homography(python_left, python_right);
  if (native_h.empty() || python_h.empty()) {
    std::cerr << "FAIL: matcher behavioral parity did not produce stable homographies\n";
    return 1;
  }
  const std::vector<cv::Point2f> probes = {
      {0.0f, 0.0f},
      {static_cast<float>(left.cols - 1), 0.0f},
      {0.0f, static_cast<float>(left.rows - 1)},
      {static_cast<float>(left.cols - 1), static_cast<float>(left.rows - 1)},
      {left.cols * 0.5f, left.rows * 0.5f},
  };
  std::vector<cv::Point2f> native_projection;
  std::vector<cv::Point2f> python_projection;
  cv::perspectiveTransform(probes, native_projection, native_h);
  cv::perspectiveTransform(probes, python_projection, python_h);
  double maximum_projection_delta = 0.0;
  for (size_t i = 0; i < probes.size(); ++i) {
    maximum_projection_delta =
        std::max(maximum_projection_delta, cv::norm(native_projection[i] - python_projection[i]));
  }
  const double diagonal = std::hypot(static_cast<double>(right.cols), static_cast<double>(right.rows));
  if (maximum_projection_delta > diagonal * 0.05) {
    std::cerr << "FAIL: ALIKED native/SuperPoint legacy stitch geometry delta=" << maximum_projection_delta << '\n';
    return 1;
  }
  return 0;
}
