#include "hstream/src/libs/stitching/Orientation.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <opencv2/core.hpp>
#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/GameConfig.h"

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void touch(const fs::path& path) {
  fs::create_directories(path.parent_path());
  std::ofstream(path).put('\0');
}

} // namespace

int main() {
  bool ok = true;
  cv::Mat mask = cv::Mat::zeros(40, 80, CV_8U);
  mask(cv::Rect(70, 0, 10, 40)).setTo(255);
  auto scores = hm::stitching::rink_orientation_scores(mask);
  ok &= expect(scores.ok() && scores->right == 400.0 && scores->left == 0.0, "edge sums must use outer eighths");
  auto orientation = hm::stitching::classify_rink_orientation(mask);
  ok &= expect(orientation.ok() && *orientation == "left", "ice on right edge means left camera");
  cv::Mat ambiguous = cv::Mat::zeros(40, 80, CV_8U);
  ok &= expect(!hm::stitching::classify_rink_orientation(ambiguous).ok(), "equal edge sums must be ambiguous");
  ok &= expect(!hm::stitching::rink_orientation_scores(cv::Mat::zeros(4, 4, CV_8U)).ok(), "tiny masks must fail");

  char template_path[] = "/tmp/hstream-orientation-test-XXXXXX";
  const char* created = ::mkdtemp(template_path);
  ok &= expect(created != nullptr, "temporary directory must be created");
  if (created != nullptr) {
    const fs::path root(created);
    touch(root / "cam10" / "GX020123.MP4");
    touch(root / "cam10" / "GX010123.MP4");
    touch(root / "cam2" / "VID_20260102_030405_002.mp4");
    touch(root / "cam2" / "VID_20260102_030405_001.mp4");
    auto videos = hm::stitching::get_available_videos(root.string());
    ok &= expect(videos.ok() && videos->size() == 2, "camN directories must be discovered");
    if (videos.ok()) {
      ok &=
          expect(videos->begin()->first == "cam10" || videos->begin()->first == "cam2", "camera keys must be retained");
      ok &=
          expect(videos->at("cam2").size() == 2 && videos->at("cam10").size() == 2, "vendor chapters must be retained");
      ok &= expect(
          fs::path(videos->at("cam10").at(1)).filename() == "GX010123.MP4",
          "GoPro chapters must sort by chapter number");
    }
    std::error_code error;
    fs::remove_all(root, error);

    const fs::path lr_root = std::string(created) + "-lr";
    touch(lr_root / "left-12.mkv");
    touch(lr_root / "left-2.mkv");
    touch(lr_root / "right-12.m4v");
    touch(lr_root / "right-2.m4v");
    auto lr = hm::stitching::get_available_videos(lr_root.string());
    ok &= expect(lr.ok() && lr->count("left") && lr->count("right"), "mkv/m4v left-right chapters must be supported");
    if (lr.ok()) {
      ok &= expect(lr->at("left").count(2) && lr->at("left").count(12), "multi-digit chapter numbers must be parsed");
    }
    fs::remove_all(lr_root, error);

    const fs::path completed_root = std::string(created) + "-completed-owner";
    fs::create_directories(completed_root / "cam1");
    fs::create_directories(completed_root / "cam2");
    const fs::path left_path = completed_root / "cam1" / "left.mp4";
    const fs::path right_path = completed_root / "cam2" / "right.mp4";
    touch(left_path);
    touch(right_path);
    YAML::Node completed(YAML::NodeType::Map);
    completed["hstream_ui"]["stitching_calibration"]["status"] = "complete";
    completed["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "program-owner";
    ok &= expect(
        hm::stitching::publish_game_config(completed_root, YAML::Dump(completed) + "\n").ok(),
        "completed Program owner fixture must publish");
    const hm::stitching::VideoChapter left{{1, left_path.string()}};
    const hm::stitching::VideoChapter right{{1, right_path.string()}};
    const auto completed_save =
        hm::stitching::orientation_internal::save_orientation_config(completed_root, left, right, "program-owner");
    const YAML::Node after_completed = YAML::LoadFile((completed_root / "config.yaml").string());
    ok &= expect(
        completed_save.ok() && after_completed["game"]["videos"]["left"][0].as<std::string>() == "cam1/left.mp4" &&
            after_completed["game"]["videos"]["right"][0].as<std::string>() == "cam2/right.mp4",
        "a completed Program generation must be allowed to persist its derived camera orientation");
    completed["hstream_ui"]["stitching_calibration"]["status"] = "pending";
    completed["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "newer-owner";
    ok &= expect(
        hm::stitching::publish_game_config(completed_root, YAML::Dump(completed) + "\n").ok(),
        "newer orientation owner fixture must publish");
    const auto superseded_save =
        hm::stitching::orientation_internal::save_orientation_config(completed_root, left, right, "program-owner");
    YAML::Node after_superseded = YAML::LoadFile((completed_root / "config.yaml").string());
    const bool stale_orientation_absent = !after_superseded["game"] || !after_superseded["game"]["videos"];
    ok &= expect(
        superseded_save.code() == absl::StatusCode::kAborted &&
            after_superseded["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() ==
                "newer-owner" &&
            stale_orientation_absent,
        "a superseded Program generation must not persist stale camera orientation");
    fs::remove_all(completed_root, error);
  }
  return ok ? 0 : 1;
}
