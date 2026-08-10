#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct VisualMatchResult {
  int panorama_keypoints = 0;
  int frame_keypoints = 0;
  int ratio_matches = 0;
  int homography_inliers = 0;
  double inlier_fraction = 0.0;
  double sampled_video_second = 0.0;
  cv::Mat frame;
  cv::Mat match_visualization;
};

cv::Mat scaled_for_match(const cv::Mat& image, int max_dimension = 1600) {
  const int largest = std::max(image.cols, image.rows);
  if (largest <= max_dimension) {
    return image.clone();
  }
  const double scale = static_cast<double>(max_dimension) / largest;
  cv::Mat scaled;
  cv::resize(image, scaled, {}, scale, scale, cv::INTER_AREA);
  return scaled;
}

VisualMatchResult compare_video_to_panorama(const fs::path& video_path, const fs::path& panorama_path) {
  VisualMatchResult best;
  ::setenv("OPENCV_FFMPEG_LOGLEVEL", "-8", /*overwrite=*/false);
  const cv::Mat panorama = cv::imread(panorama_path.string(), cv::IMREAD_COLOR);
  cv::VideoCapture video(video_path.string());
  if (panorama.empty() || !video.isOpened()) {
    return best;
  }

  const cv::Mat panorama_scaled = scaled_for_match(panorama);
  cv::Mat panorama_gray;
  cv::cvtColor(panorama_scaled, panorama_gray, cv::COLOR_BGR2GRAY);
  cv::Mat panorama_mask;
  cv::threshold(panorama_gray, panorama_mask, 8, 255, cv::THRESH_BINARY);
  const cv::Ptr<cv::SIFT> detector = cv::SIFT::create(5000);
  std::vector<cv::KeyPoint> panorama_keypoints;
  cv::Mat panorama_descriptors;
  detector->detectAndCompute(panorama_gray, panorama_mask, panorama_keypoints, panorama_descriptors);
  best.panorama_keypoints = static_cast<int>(panorama_keypoints.size());
  if (panorama_descriptors.empty()) {
    return best;
  }

  const double frame_count = video.get(cv::CAP_PROP_FRAME_COUNT);
  const double fps = video.get(cv::CAP_PROP_FPS);
  auto consider_frame = [&](const cv::Mat& frame, double sampled_frame) {
    const cv::Mat frame_scaled = scaled_for_match(frame);
    cv::Mat frame_gray;
    cv::cvtColor(frame_scaled, frame_gray, cv::COLOR_BGR2GRAY);
    cv::Scalar mean;
    cv::Scalar deviation;
    cv::meanStdDev(frame_gray, mean, deviation);
    if (deviation[0] < 5.0) {
      return;
    }
    std::vector<cv::KeyPoint> frame_keypoints;
    cv::Mat frame_descriptors;
    detector->detectAndCompute(frame_gray, cv::noArray(), frame_keypoints, frame_descriptors);
    if (frame_descriptors.empty()) {
      return;
    }

    std::vector<std::vector<cv::DMatch>> neighbors;
    cv::BFMatcher(cv::NORM_L2).knnMatch(frame_descriptors, panorama_descriptors, neighbors, 2);
    std::vector<cv::DMatch> ratio_matches;
    for (const auto& pair : neighbors) {
      if (pair.size() == 2 && pair[0].distance < 0.75f * pair[1].distance) {
        ratio_matches.push_back(pair[0]);
      }
    }

    std::vector<unsigned char> inlier_mask;
    int inliers = 0;
    if (ratio_matches.size() >= 4) {
      std::vector<cv::Point2f> frame_points;
      std::vector<cv::Point2f> panorama_points;
      frame_points.reserve(ratio_matches.size());
      panorama_points.reserve(ratio_matches.size());
      for (const cv::DMatch& match : ratio_matches) {
        frame_points.push_back(frame_keypoints[match.queryIdx].pt);
        panorama_points.push_back(panorama_keypoints[match.trainIdx].pt);
      }
      cv::findHomography(frame_points, panorama_points, cv::RANSAC, 5.0, inlier_mask);
      inliers = static_cast<int>(std::count(inlier_mask.begin(), inlier_mask.end(), static_cast<unsigned char>(1)));
    }
    const double inlier_fraction = ratio_matches.empty() ? 0.0 : static_cast<double>(inliers) / ratio_matches.size();
    const double candidate_score = inliers * inlier_fraction;
    const double best_score = best.homography_inliers * best.inlier_fraction;
    if (candidate_score <= best_score) {
      return;
    }

    best.frame = frame;
    best.frame_keypoints = static_cast<int>(frame_keypoints.size());
    best.ratio_matches = static_cast<int>(ratio_matches.size());
    best.homography_inliers = inliers;
    best.inlier_fraction = inlier_fraction;
    best.sampled_video_second = fps > 0.0 ? sampled_frame / fps : 0.0;

    std::vector<cv::DMatch> inlier_matches;
    for (size_t i = 0; i < ratio_matches.size() && inlier_matches.size() < 100; ++i) {
      if (i < inlier_mask.size() && inlier_mask[i]) {
        inlier_matches.push_back(ratio_matches[i]);
      }
    }
    cv::drawMatches(
        frame_scaled,
        frame_keypoints,
        panorama_scaled,
        panorama_keypoints,
        inlier_matches,
        best.match_visualization,
        cv::Scalar::all(-1),
        cv::Scalar::all(-1),
        {},
        cv::DrawMatchesFlags::NOT_DRAW_SINGLE_POINTS);
  };

  // Container metadata can advertise the audio duration even when a short UI
  // smoke run has only encoded the first few video seconds. Try ordinary
  // fractional seeks first, then always scan the early decoded video at
  // half-second intervals. The latter also covers OpenCV/FFmpeg builds that
  // report CAP_PROP_FRAME_COUNT as zero or cannot seek this HEVC Matroska
  // stream accurately.
  for (double fraction : {0.15, 0.5, 0.85}) {
    if (frame_count <= 1) {
      break;
    }
    video.set(cv::CAP_PROP_POS_FRAMES, std::floor((frame_count - 1) * fraction));
    cv::Mat frame;
    if (!video.read(frame) || frame.empty()) {
      continue;
    }
    consider_frame(frame, std::max(0.0, video.get(cv::CAP_PROP_POS_FRAMES) - 1.0));
  }

  video.release();
  video.open(video_path.string());
  if (video.isOpened()) {
    const int sample_stride = std::max(1, static_cast<int>(std::round(fps > 0.0 ? fps / 2.0 : 15.0)));
    const int maximum_frames =
        std::max(sample_stride * 12, static_cast<int>(std::round(fps > 0.0 ? fps * 8.0 : 240.0)));
    for (int frame_index = 0; frame_index < maximum_frames; ++frame_index) {
      cv::Mat frame;
      if (!video.read(frame) || frame.empty()) {
        break;
      }
      if (frame_index % sample_stride == 0) {
        consider_frame(frame, frame_index);
      }
    }
  }
  return best;
}

int minimum_inliers() {
  const char* configured = std::getenv("HSTREAM_UI_E2E_MIN_VISUAL_INLIERS");
  if (!configured || !*configured) {
    return 20;
  }
  try {
    return std::max(1, std::stoi(configured));
  } catch (...) {
    return 20;
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " VIDEO PANORAMA_TIF ARTIFACT_DIR\n";
    return 2;
  }
  const fs::path video_path(argv[1]);
  const fs::path panorama_path(argv[2]);
  const fs::path artifact_dir(argv[3]);
  std::error_code error;
  fs::create_directories(artifact_dir, error);
  if (error || !fs::is_regular_file(video_path) || !fs::is_regular_file(panorama_path)) {
    std::cerr << "Visual verification inputs are unavailable\n";
    return 2;
  }

  const VisualMatchResult result = compare_video_to_panorama(video_path, panorama_path);
  const bool passed = result.homography_inliers >= minimum_inliers() && result.inlier_fraction >= 0.35;
  if (!result.frame.empty()) {
    cv::imwrite((artifact_dir / "encoded-frame.jpg").string(), result.frame);
  }
  if (!result.match_visualization.empty()) {
    cv::imwrite((artifact_dir / "panorama-feature-matches.jpg").string(), result.match_visualization);
  }
  const cv::Mat panorama_preview = scaled_for_match(cv::imread(panorama_path.string(), cv::IMREAD_COLOR));
  if (!panorama_preview.empty()) {
    cv::imwrite((artifact_dir / "panorama-reference.jpg").string(), panorama_preview);
  }

  std::ostringstream report;
  report << "panorama_keypoints: " << result.panorama_keypoints << '\n';
  report << "frame_keypoints: " << result.frame_keypoints << '\n';
  report << "ratio_matches: " << result.ratio_matches << '\n';
  report << "homography_inliers: " << result.homography_inliers << '\n';
  report << "inlier_fraction: " << result.inlier_fraction << '\n';
  report << "sampled_video_second: " << result.sampled_video_second << '\n';
  report << "minimum_inliers: " << minimum_inliers() << '\n';
  report << "visual_match: " << (passed ? "PASS" : "FAIL") << '\n';
  std::ofstream(artifact_dir / "visual-report.txt") << report.str();
  std::cout << report.str();
  return passed ? 0 : 1;
}
