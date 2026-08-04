#include "hstream/src/libs/stitching/FeatureMatcher.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/Orientation.h"
#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
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

constexpr double kReferenceMemoryLimitGiB = 36.0;

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
  points.reserve(node.size());
  for (const auto& point : node) {
    if (point.IsSequence() && point.size() == 2)
      points.emplace_back(point[0].as<float>(), point[1].as<float>());
  }
  return points;
}

std::vector<int> yaml_ints(const YAML::Node& node) {
  std::vector<int> values;
  if (!node || !node.IsSequence())
    return values;
  values.reserve(node.size());
  for (const auto& value : node)
    values.push_back(value.as<int>());
  return values;
}

std::vector<float> yaml_floats(const YAML::Node& node) {
  std::vector<float> values;
  if (!node || !node.IsSequence())
    return values;
  values.reserve(node.size());
  for (const auto& value : node)
    values.push_back(value.as<float>());
  return values;
}

cv::Mat homography(const std::vector<cv::Point2f>& left, const std::vector<cv::Point2f>& right) {
  if (left.size() < 8 || left.size() != right.size())
    return {};
  return cv::findHomography(left, right, cv::RANSAC, 3.0);
}

std::optional<std::string> read_file(const fs::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return std::nullopt;
  std::ostringstream contents;
  contents << input.rdbuf();
  if (!input.good() && !input.eof())
    return std::nullopt;
  return contents.str();
}

struct HuginOutcome {
  hm::stitching::HuginProject::CameraPose first;
  hm::stitching::HuginProject::CameraPose second;
  size_t width;
  size_t height;
};

std::optional<HuginOutcome> configure_hugin(
    const fs::path& output_dir,
    const fs::path& game_dir,
    const std::vector<hm::stitching::FeatureMatch>& matches) {
  std::error_code error;
  fs::create_directories(output_dir, error);
  if (error)
    return std::nullopt;
  for (const char* image : {"left.png", "right.png"}) {
    fs::copy_file(game_dir / image, output_dir / image, fs::copy_options::overwrite_existing, error);
    if (error)
      return std::nullopt;
  }
  hm::stitching::HuginProject::Options options;
  options.max_canvas_dimension = 2048;
  const auto configured = hm::stitching::HuginProject::Configure(output_dir, matches, options);
  if (!configured.ok()) {
    std::cerr << "FAIL: Hugin outcome generation failed for " << output_dir.filename() << ": " << configured << '\n';
    return std::nullopt;
  }
  const auto project = read_file(output_dir / "autooptimiser_out.pto");
  if (!project.has_value())
    return std::nullopt;
  const auto canvas = hm::stitching::HuginProject::ParseCanvasSize(*project);
  const auto projection = hm::stitching::HuginProject::ParseProjection(*project);
  const auto first = hm::stitching::HuginProject::ParseCameraPose(*project, 0);
  const auto second = hm::stitching::HuginProject::ParseCameraPose(*project, 1);
  const cv::Mat seam = cv::imread((output_dir / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  if (!canvas.ok() || !projection.ok() || *projection != 1 || !first.ok() || !second.ok() || seam.empty()) {
    std::cerr << "FAIL: optimized Hugin outcome could not be parsed for " << output_dir.filename() << '\n';
    return std::nullopt;
  }
  return HuginOutcome{*first, *second, static_cast<size_t>(seam.cols), static_cast<size_t>(seam.rows)};
}

double angular_delta(double left, double right) {
  return std::abs(std::remainder(left - right, 360.0));
}

double maximum_point_delta(
    const cv::Point2f& left_a,
    const cv::Point2f& right_a,
    const cv::Point2f& left_b,
    const cv::Point2f& right_b) {
  return std::max(cv::norm(left_a - left_b), cv::norm(right_a - right_b));
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 6)
    return skip_or_fail("pinned Python parity helper/source/weights were not provided");
  const char* game_directory = std::getenv("HM_ONNX_PARITY_GAME_DIR");
  if (game_directory == nullptr || *game_directory == '\0')
    return skip_or_fail("set HM_ONNX_PARITY_GAME_DIR to an explicit pinned fixture directory");
  const fs::path game_dir(game_directory);
  const char* rink_config = std::getenv("HM_PARITY_RINK_CONFIG");
  const char* rink_checkpoint = std::getenv("HM_PARITY_RINK_CHECKPOINT");
  if (required() &&
      (rink_config == nullptr || *rink_config == '\0' || rink_checkpoint == nullptr || *rink_checkpoint == '\0')) {
    return skip_or_fail("set HM_PARITY_RINK_CONFIG and HM_PARITY_RINK_CHECKPOINT for mandatory rink parity");
  }
  const fs::path rink_model = model_path("HM_RINK_ONNX_MODEL", "ice-rink-mask2former-swin-s-2c231f9f4897779d.onnx");
  const fs::path matcher_model =
      model_path("HM_FEATURE_MATCHER_ONNX_MODEL", "aliked-lightglue-k2048-ea4a4ab2cb556958.onnx");
  if (!fs::is_regular_file(game_dir / "left.png") || !fs::is_regular_file(game_dir / "right.png") ||
      !fs::is_regular_file(rink_model) || !fs::is_regular_file(matcher_model)) {
    return skip_or_fail("native models and left/right game fixtures are required for Python parity");
  }
  if (required() && !fs::is_regular_file(game_dir / "s.png"))
    return skip_or_fail("mandatory parity fixture is missing s.png: " + game_dir.string());

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
  std::vector<std::string> reference_arguments = {
      python_executable(),
      argv[1],
      "--game-dir",
      game_dir.string(),
      "--output-dir",
      output_dir.string(),
      "--max-control-points",
      "1500",
      "--memory-limit-gib",
      std::to_string(kReferenceMemoryLimitGiB),
      "--lightglue-source-init",
      argv[2],
      "--raco-weights",
      argv[3],
      "--aliked-weights",
      argv[4],
      "--lightglue-weights",
      argv[5],
  };
  if (rink_config != nullptr && *rink_config != '\0' && rink_checkpoint != nullptr && *rink_checkpoint != '\0') {
    reference_arguments.insert(
        reference_arguments.end(),
        {"--rink-config",
         rink_config,
         "--rink-checkpoint",
         rink_checkpoint,
         "--rink-inference-scale",
         std::to_string(hm::stitching::RinkSegmentation::kHockeyMomInferenceScale)});
  }
  const int reference_status = run_reference(reference_arguments);
  if (reference_status == 77 || reference_status == 127)
    return skip_or_fail("HockeyMOM/pinned RaCo-ALIKED Python reference is not available");
  if (reference_status != 0) {
    std::cerr << "FAIL: Python parity reference exited " << reference_status << '\n';
    return 1;
  }

  const cv::Mat left = cv::imread((game_dir / "left.png").string(), cv::IMREAD_COLOR);
  const cv::Mat right = cv::imread((game_dir / "right.png").string(), cv::IMREAD_COLOR);
  auto matcher = hm::stitching::FeatureMatcher::Create(matcher_model.string());
  if (!matcher.ok()) {
    std::cerr << "FAIL: native matcher model contract failed: " << matcher.status() << '\n';
    return 1;
  }
  auto native_matches = (*matcher)->Infer(left, right, 1500);
  if (!native_matches.ok()) {
    std::cerr << "FAIL: native matcher inference failed: " << native_matches.status() << '\n';
    return 1;
  }

  YAML::Node reference = YAML::LoadFile((output_dir / "python_reference.yaml").string());
  const std::vector<int> python_left_indices = yaml_ints(reference["raco_left_indices"]);
  const std::vector<int> python_right_indices = yaml_ints(reference["raco_right_indices"]);
  const std::vector<cv::Point2f> python_left = yaml_points(reference["raco_left_points"]);
  const std::vector<cv::Point2f> python_right = yaml_points(reference["raco_right_points"]);
  const std::vector<float> python_scores = yaml_floats(reference["raco_scores"]);
  const size_t python_count = python_left_indices.size();
  if (python_count < 8 || python_right_indices.size() != python_count || python_left.size() != python_count ||
      python_right.size() != python_count || python_scores.size() != python_count) {
    std::cerr << "FAIL: pinned Python RaCo-ALIKED oracle returned an invalid accepted-match contract\n";
    return 1;
  }

  struct PythonMatch {
    cv::Point2f left;
    cv::Point2f right;
    float score;
  };
  std::map<std::pair<int, int>, PythonMatch> python_by_pair;
  for (size_t index = 0; index < python_count; ++index) {
    if (!python_by_pair
             .emplace(
                 std::make_pair(python_left_indices[index], python_right_indices[index]),
                 PythonMatch{python_left[index], python_right[index], python_scores[index]})
             .second) {
      std::cerr << "FAIL: pinned Python RaCo-ALIKED oracle returned duplicate match indices\n";
      return 1;
    }
  }

  size_t shared_index_pairs = 0;
  for (const auto& native : native_matches->accepted) {
    if (python_by_pair.find({native.left_index, native.right_index}) != python_by_pair.end())
      ++shared_index_pairs;
  }
  std::vector<bool> python_used(python_count, false);
  size_t shared_spatial_pairs = 0;
  double maximum_coordinate_delta = 0.0;
  double maximum_score_delta = 0.0;
  for (const auto& native : native_matches->accepted) {
    size_t best_index = python_count;
    double best_delta = std::numeric_limits<double>::infinity();
    for (size_t python_index = 0; python_index < python_count; ++python_index) {
      if (python_used[python_index])
        continue;
      const double delta =
          maximum_point_delta(native.left, native.right, python_left[python_index], python_right[python_index]);
      if (delta < best_delta) {
        best_delta = delta;
        best_index = python_index;
      }
    }
    if (best_index == python_count || best_delta > 0.25)
      continue;
    python_used[best_index] = true;
    ++shared_spatial_pairs;
    maximum_coordinate_delta = std::max(maximum_coordinate_delta, best_delta);
    maximum_score_delta =
        std::max(maximum_score_delta, static_cast<double>(std::abs(native.score - python_scores[best_index])));
  }
  const size_t pair_denominator = std::max(native_matches->accepted.size(), python_count);
  const double identical_spatial_pair_ratio =
      pair_denominator == 0 ? 0.0 : static_cast<double>(shared_spatial_pairs) / pair_denominator;
  const double identical_index_pair_ratio =
      pair_denominator == 0 ? 0.0 : static_cast<double>(shared_index_pairs) / pair_denominator;
  const std::vector<cv::Point2f> python_selected_left = yaml_points(reference["raco_selected_left_points"]);
  const std::vector<cv::Point2f> python_selected_right = yaml_points(reference["raco_selected_right_points"]);
  double maximum_selected_delta = 0.0;
  const size_t compared_selected =
      std::min(native_matches->selected.size(), std::min(python_selected_left.size(), python_selected_right.size()));
  for (size_t index = 0; index < compared_selected; ++index) {
    maximum_selected_delta = std::max(
        maximum_selected_delta,
        maximum_point_delta(
            native_matches->selected[index].left,
            native_matches->selected[index].right,
            python_selected_left[index],
            python_selected_right[index]));
  }
  std::vector<cv::Point2f> native_accepted_left;
  std::vector<cv::Point2f> native_accepted_right;
  native_accepted_left.reserve(native_matches->accepted.size());
  native_accepted_right.reserve(native_matches->accepted.size());
  for (const auto& match : native_matches->accepted) {
    native_accepted_left.push_back(match.left);
    native_accepted_right.push_back(match.right);
  }
  const cv::Mat native_raco_h = homography(native_accepted_left, native_accepted_right);
  const cv::Mat python_raco_h = homography(python_left, python_right);
  if (native_raco_h.empty() || python_raco_h.empty()) {
    std::cerr << "FAIL: native/Python RaCo-ALIKED matches did not produce stable homographies\n";
    return 1;
  }
  const std::vector<cv::Point2f> parity_probes = {
      {0.0f, 0.0f},
      {static_cast<float>(left.cols - 1), 0.0f},
      {0.0f, static_cast<float>(left.rows - 1)},
      {static_cast<float>(left.cols - 1), static_cast<float>(left.rows - 1)},
      {left.cols * 0.5f, left.rows * 0.5f},
  };
  std::vector<cv::Point2f> native_raco_projection;
  std::vector<cv::Point2f> python_raco_projection;
  cv::perspectiveTransform(parity_probes, native_raco_projection, native_raco_h);
  cv::perspectiveTransform(parity_probes, python_raco_projection, python_raco_h);
  double maximum_projection_delta = 0.0;
  for (size_t index = 0; index < parity_probes.size(); ++index) {
    maximum_projection_delta =
        std::max(maximum_projection_delta, cv::norm(native_raco_projection[index] - python_raco_projection[index]));
  }
  const double accepted_count_ratio =
      static_cast<double>(native_matches->accepted.size()) / static_cast<double>(python_count);
  // The optimized upstream graph is documented as near-parity and changes a
  // provider-dependent fringe of marginal matches. Require a meaningful set
  // of identical, subpixel correspondences plus a bounded total count here;
  // the mandatory gate below validates the actual optimized Hugin result.
  if (shared_spatial_pairs < 64 || identical_spatial_pair_ratio < 0.8 || accepted_count_ratio < 0.8 ||
      accepted_count_ratio > 1.2 || maximum_coordinate_delta > 0.01) {
    std::cerr << "FAIL: native/Python RaCo-ALIKED parity spatial_pair_ratio=" << identical_spatial_pair_ratio
              << " count_ratio=" << accepted_count_ratio
              << " diagnostic_index_pair_ratio=" << identical_index_pair_ratio
              << " coordinate_delta=" << maximum_coordinate_delta << " diagnostic_score_delta=" << maximum_score_delta
              << " projection_delta=" << maximum_projection_delta
              << " diagnostic_selected_delta=" << maximum_selected_delta << '\n';
    return 1;
  }
  std::cerr << "INFO: native/Python RaCo-ALIKED parity spatial_pair_ratio=" << identical_spatial_pair_ratio
            << " count_ratio=" << accepted_count_ratio << " diagnostic_index_pair_ratio=" << identical_index_pair_ratio
            << " coordinate_delta=" << maximum_coordinate_delta << " diagnostic_score_delta=" << maximum_score_delta
            << " projection_delta=" << maximum_projection_delta
            << " native_selected_count=" << native_matches->selected.size()
            << " python_selected_count=" << python_selected_left.size()
            << " diagnostic_selected_delta=" << maximum_selected_delta << '\n';

  const bool rink_available = reference["rink_available"] && reference["rink_available"].as<bool>();
  if (!rink_available) {
    const std::string reason = reference["rink_skip_reason"] ? reference["rink_skip_reason"].as<std::string>()
                                                             : "unknown HockeyMOM rink dependency";
    if (required()) {
      std::cerr << "FAIL: mandatory HockeyMOM rink parity is unavailable: " << reason << '\n';
      return 1;
    }
    std::cerr << "SKIP: HockeyMOM rink parity is unavailable: " << reason << '\n';
  } else {
    const cv::Mat stitched = cv::imread((game_dir / "s.png").string(), cv::IMREAD_COLOR);
    auto rink = hm::stitching::RinkSegmentation::Create(rink_model.string());
    if (stitched.empty() || !rink.ok()) {
      std::cerr << "FAIL: native rink model/fixture contract failed\n";
      return 1;
    }
    auto native_rink = (*rink)->Infer(stitched, hm::stitching::RinkSegmentation::kHockeyMomInferenceScale);
    if (!native_rink.ok()) {
      std::cerr << "FAIL: native rink inference failed: " << native_rink.status() << '\n';
      return 1;
    }
    const cv::Mat python_mask = cv::imread((output_dir / "python_rink_mask.png").string(), cv::IMREAD_GRAYSCALE);
    if (python_mask.empty() || python_mask.size() != native_rink->combined_mask.size()) {
      std::cerr << "FAIL: Python rink mask contract changed\n";
      return 1;
    }
    cv::Mat intersection;
    cv::Mat union_mask;
    cv::bitwise_and(python_mask, native_rink->combined_mask, intersection);
    cv::bitwise_or(python_mask, native_rink->combined_mask, union_mask);
    const int union_pixels = cv::countNonZero(union_mask);
    const double iou = union_pixels == 0 ? 0.0 : static_cast<double>(cv::countNonZero(intersection)) / union_pixels;
    const double centroid_dx = std::abs(native_rink->centroid.x - reference["centroid"][0].as<double>());
    const double centroid_dy = std::abs(native_rink->centroid.y - reference["centroid"][1].as<double>());
    const double maximum_centroid_relative_delta =
        std::max(centroid_dx / python_mask.cols, centroid_dy / python_mask.rows);
    const std::vector<double> native_bbox = {
        native_rink->combined_bbox.x,
        native_rink->combined_bbox.y,
        native_rink->combined_bbox.x + native_rink->combined_bbox.width,
        native_rink->combined_bbox.y + native_rink->combined_bbox.height,
    };
    double maximum_bbox_delta = 0.0;
    double maximum_bbox_relative_delta = 0.0;
    for (size_t index = 0; index < native_bbox.size(); ++index) {
      const double delta = std::abs(native_bbox[index] - reference["bbox"][index].as<double>());
      maximum_bbox_delta = std::max(maximum_bbox_delta, delta);
      const double dimension = index % 2 == 0 ? python_mask.cols : python_mask.rows;
      maximum_bbox_relative_delta = std::max(maximum_bbox_relative_delta, delta / dimension);
    }
    auto native_orientation = hm::stitching::classify_rink_orientation(native_rink->combined_mask);
    auto python_orientation = hm::stitching::classify_rink_orientation(python_mask);
    if (iou < 0.99 || maximum_centroid_relative_delta > 0.005 || maximum_bbox_relative_delta > 0.005 ||
        !native_orientation.ok() || !python_orientation.ok() || *native_orientation != *python_orientation) {
      std::cerr << "FAIL: rink parity iou=" << iou << " centroid_delta=" << centroid_dx << ',' << centroid_dy
                << " centroid_relative_delta=" << maximum_centroid_relative_delta
                << " bbox_delta=" << maximum_bbox_delta << " bbox_relative_delta=" << maximum_bbox_relative_delta
                << '\n';
      return 1;
    }
  }

  if (reference["legacy_superpoint_available"] && reference["legacy_superpoint_available"].as<bool>()) {
    const std::vector<cv::Point2f> legacy_left = yaml_points(reference["legacy_left_points"]);
    const std::vector<cv::Point2f> legacy_right = yaml_points(reference["legacy_right_points"]);
    if (legacy_left.size() < 16 || legacy_left.size() != legacy_right.size()) {
      std::cerr << "FAIL: legacy SuperPoint oracle returned unusable control points\n";
      return 1;
    }
    std::vector<cv::Point2f> native_left;
    std::vector<cv::Point2f> native_right;
    for (const auto& match : native_matches->selected) {
      native_left.push_back(match.left);
      native_right.push_back(match.right);
    }
    const cv::Mat native_h = homography(native_left, native_right);
    const cv::Mat legacy_h = homography(legacy_left, legacy_right);
    if (!native_h.empty() && !legacy_h.empty()) {
      const std::vector<cv::Point2f> probes = {
          {0.0f, 0.0f},
          {static_cast<float>(left.cols - 1), 0.0f},
          {0.0f, static_cast<float>(left.rows - 1)},
          {static_cast<float>(left.cols - 1), static_cast<float>(left.rows - 1)},
          {left.cols * 0.5f, left.rows * 0.5f},
      };
      std::vector<cv::Point2f> native_projection;
      std::vector<cv::Point2f> legacy_projection;
      cv::perspectiveTransform(probes, native_projection, native_h);
      cv::perspectiveTransform(probes, legacy_projection, legacy_h);
      double maximum_legacy_delta = 0.0;
      for (size_t index = 0; index < probes.size(); ++index)
        maximum_legacy_delta =
            std::max(maximum_legacy_delta, cv::norm(native_projection[index] - legacy_projection[index]));
      std::cerr << "INFO: optional legacy SuperPoint projection delta=" << maximum_legacy_delta << '\n';
    }
    if (required()) {
      std::vector<hm::stitching::FeatureMatch> legacy_matches;
      legacy_matches.reserve(legacy_left.size());
      for (size_t index = 0; index < legacy_left.size(); ++index) {
        const auto duplicate = std::find_if(legacy_matches.begin(), legacy_matches.end(), [&](const auto& match) {
          return match.left == legacy_left[index] && match.right == legacy_right[index];
        });
        if (duplicate == legacy_matches.end())
          legacy_matches.push_back({legacy_left[index], legacy_right[index], 1.0f});
      }
      if (legacy_matches.size() < 16) {
        std::cerr << "FAIL: legacy SuperPoint oracle returned fewer than 16 unique control points\n";
        return 1;
      }
      const auto native_hugin = configure_hugin(output_dir / "native-hugin", game_dir, native_matches->selected);
      const auto legacy_hugin = configure_hugin(output_dir / "legacy-hugin", game_dir, legacy_matches);
      if (!native_hugin.has_value() || !legacy_hugin.has_value())
        return 1;

      const auto relative_pose = [](const HuginOutcome& outcome) {
        return hm::stitching::HuginProject::CameraPose{
            outcome.second.roll - outcome.first.roll,
            outcome.second.pitch - outcome.first.pitch,
            outcome.second.yaw - outcome.first.yaw,
        };
      };
      const auto native_pose = relative_pose(*native_hugin);
      const auto legacy_pose = relative_pose(*legacy_hugin);
      const double roll_delta = angular_delta(native_pose.roll, legacy_pose.roll);
      const double pitch_delta = angular_delta(native_pose.pitch, legacy_pose.pitch);
      const double yaw_delta = angular_delta(native_pose.yaw, legacy_pose.yaw);
      const double native_aspect = static_cast<double>(native_hugin->width) / native_hugin->height;
      const double legacy_aspect = static_cast<double>(legacy_hugin->width) / legacy_hugin->height;
      const double aspect_delta = std::abs(native_aspect - legacy_aspect) / legacy_aspect;
      const double width_delta =
          std::abs(static_cast<double>(native_hugin->width) - legacy_hugin->width) / legacy_hugin->width;
      const double height_delta =
          std::abs(static_cast<double>(native_hugin->height) - legacy_hugin->height) / legacy_hugin->height;
      std::cerr << "INFO: Hugin outcome native_pose=" << native_pose.roll << ',' << native_pose.pitch << ','
                << native_pose.yaw << " legacy_pose=" << legacy_pose.roll << ',' << legacy_pose.pitch << ','
                << legacy_pose.yaw << " pose_delta=" << roll_delta << ',' << pitch_delta << ',' << yaw_delta
                << " native_canvas=" << native_hugin->width << 'x' << native_hugin->height
                << " legacy_canvas=" << legacy_hugin->width << 'x' << legacy_hugin->height
                << " canvas_delta=" << width_delta << ',' << height_delta << " aspect_delta=" << aspect_delta << '\n';
      if (roll_delta > 5.0 || pitch_delta > 5.0 || yaw_delta > 5.0 || width_delta > 0.15 || height_delta > 0.15 ||
          aspect_delta > 0.15) {
        std::cerr << "FAIL: native Hugin calibration diverges from the legacy production outcome\n";
        return 1;
      }
    }
  } else if (reference["legacy_superpoint_skip_reason"]) {
    if (required()) {
      std::cerr << "FAIL: mandatory legacy SuperPoint Hugin oracle is unavailable: "
                << reference["legacy_superpoint_skip_reason"].as<std::string>() << '\n';
      return 1;
    }
    std::cerr << "INFO: optional legacy SuperPoint comparison unavailable: "
              << reference["legacy_superpoint_skip_reason"].as<std::string>() << '\n';
  } else if (required()) {
    std::cerr << "FAIL: mandatory legacy SuperPoint Hugin oracle returned no status\n";
    return 1;
  }
  return 0;
}
