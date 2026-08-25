#include "hstream/src/libs/stitching/HomographyMaps.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <tiffio.h>
#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool has_tiff_placement(const std::filesystem::path& path, uint32_t width, uint32_t height, float x, float y) {
  TIFF* tif = TIFFOpen(path.c_str(), "r");
  if (tif == nullptr)
    return false;
  uint32_t actual_width = 0;
  uint32_t actual_height = 0;
  float xres = 0.0f;
  float yres = 0.0f;
  float xpos = 0.0f;
  float ypos = 0.0f;
  const bool ok = TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &actual_width) &&
      TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &actual_height) && TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres) &&
      TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres) && TIFFGetField(tif, TIFFTAG_XPOSITION, &xpos) &&
      TIFFGetField(tif, TIFFTAG_YPOSITION, &ypos);
  TIFFClose(tif);
  return ok && actual_width == width && actual_height == height && std::abs(xpos * xres - x) < 1e-4f &&
      std::abs(ypos * yres - y) < 1e-4f;
}

struct TiffPlacement {
  int x{0};
  int y{0};
};

TiffPlacement read_tiff_placement(const std::filesystem::path& path) {
  TIFF* tif = TIFFOpen(path.c_str(), "r");
  uint32_t width = 0;
  uint32_t height = 0;
  float xres = 1.0f;
  float yres = 1.0f;
  float xpos = 0.0f;
  float ypos = 0.0f;
  TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &width);
  TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &height);
  TIFFGetField(tif, TIFFTAG_XRESOLUTION, &xres);
  TIFFGetField(tif, TIFFTAG_YRESOLUTION, &yres);
  TIFFGetField(tif, TIFFTAG_XPOSITION, &xpos);
  TIFFGetField(tif, TIFFTAG_YPOSITION, &ypos);
  TIFFClose(tif);
  return {static_cast<int>(std::lround(xpos * xres)), static_cast<int>(std::lround(ypos * yres))};
}

bool remap_valid_at(const cv::Mat& x_map, const cv::Mat& y_map, int x, int y) {
  return x >= 0 && y >= 0 && x < x_map.cols && y < x_map.rows &&
      x_map.at<uint16_t>(y, x) != std::numeric_limits<uint16_t>::max() &&
      y_map.at<uint16_t>(y, x) != std::numeric_limits<uint16_t>::max();
}

bool seam_only_selects_valid_remaps(const std::filesystem::path& directory) {
  const cv::Mat seam = cv::imread((directory / "seam_file.png").string(), cv::IMREAD_GRAYSCALE);
  const cv::Mat left_x = cv::imread((directory / "mapping_0000_x.tif").string(), cv::IMREAD_ANYDEPTH);
  const cv::Mat left_y = cv::imread((directory / "mapping_0000_y.tif").string(), cv::IMREAD_ANYDEPTH);
  const cv::Mat right_x = cv::imread((directory / "mapping_0001_x.tif").string(), cv::IMREAD_ANYDEPTH);
  const cv::Mat right_y = cv::imread((directory / "mapping_0001_y.tif").string(), cv::IMREAD_ANYDEPTH);
  if (seam.empty() || left_x.empty() || left_y.empty() || right_x.empty() || right_y.empty())
    return false;
  const TiffPlacement left = read_tiff_placement(directory / "mapping_0000.tif");
  const TiffPlacement right = read_tiff_placement(directory / "mapping_0001.tif");
  bool checked_left = false;
  bool checked_right = false;
  for (int y = 0; y < seam.rows; ++y) {
    for (int x = 0; x < seam.cols; ++x) {
      const bool left_valid = remap_valid_at(left_x, left_y, x - left.x, y - left.y);
      const bool right_valid = remap_valid_at(right_x, right_y, x - right.x, y - right.y);
      if (!left_valid && !right_valid)
        continue;
      if (seam.at<uint8_t>(y, x) == 255) {
        if (!right_valid)
          return false;
        checked_right = true;
      } else {
        if (!left_valid)
          return false;
        checked_left = true;
      }
    }
  }
  return checked_left && checked_right;
}

} // namespace

int main() {
  namespace fs = std::filesystem;
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("hstream-homography-maps-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  fs::create_directories(root);

  cv::Mat left(80, 100, CV_8UC3, cv::Scalar(10, 20, 30));
  cv::Mat right(80, 100, CV_8UC3, cv::Scalar(40, 50, 60));
  std::vector<hm::stitching::FeatureMatch> matches;
  for (int y = 8; y < 80; y += 18) {
    for (int x = 8; x < 100; x += 18) {
      matches.push_back(
          {{static_cast<float>(x), static_cast<float>(y)},
           {static_cast<float>(x - 12), static_cast<float>(y + 4)},
           0.9f});
    }
  }
  matches.push_back({{0.0f, 0.0f}, {200.0f, 200.0f}, 0.1f});

  auto magsac =
      hm::stitching::CreateOpenCvMappingFiles(root, left, right, matches, hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(magsac.ok(), "MAGSAC mapping should generate artifacts");
  if (magsac.ok()) {
    ok &= expect(magsac->canvas_width >= 111 && magsac->canvas_width <= 113, "canvas width should include offset");
    ok &= expect(magsac->canvas_height >= 83 && magsac->canvas_height <= 85, "canvas height should include offset");
    ok &= expect(
        magsac->right_to_left_homography.size() == 9 && std::abs(magsac->right_to_left_homography[2] - 12.0) < 0.25 &&
            std::abs(magsac->right_to_left_homography[5] + 4.0) < 0.25,
        "right-to-left homography should match synthetic translation");
    ok &= expect(
        has_tiff_placement(root / "mapping_0000.tif", 100, 80, 0.0f, 4.0f),
        "left RGB mapping TIFF should carry placement metadata");
    cv::Mat x_map = cv::imread((root / "mapping_0001_x.tif").string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    cv::Mat y_map = cv::imread((root / "mapping_0001_y.tif").string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    ok &= expect(!x_map.empty() && x_map.type() == CV_16UC1, "right X remap should be uint16");
    ok &= expect(
        !y_map.empty() && y_map.type() == CV_16UC1 && y_map.size() == x_map.size(), "right Y remap should match X");
  }

  fs::path affine_dir = root / "affine";
  fs::create_directories(affine_dir);
  auto affine = hm::stitching::CreateOpenCvMappingFiles(
      affine_dir, left, right, matches, hm::stitching::MappingBackend::kOpenCvAffineRansac, 64);
  ok &= expect(affine.ok(), "affine RANSAC mapping should generate artifacts");
  if (affine.ok()) {
    ok &= expect(std::max(affine->canvas_width, affine->canvas_height) <= 64, "max canvas dimension should be honored");
    ok &= expect(affine->inlier_mask.size() == matches.size(), "affine inlier mask should cover input matches");
    cv::Mat scaled_left_x =
        cv::imread((affine_dir / "mapping_0000_x.tif").string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    ok &= expect(
        !scaled_left_x.empty() && scaled_left_x.at<uint16_t>(0, scaled_left_x.cols - 1) > 90,
        "scaled left remap should address the full source width");
  }

  ok &= expect(
      hm::stitching::ParseMappingBackend("opencv_magsac").ok(), "mapping backend parser should accept underscores");
  ok &= expect(hm::stitching::ParseMappingBackend("MAGSAC++").ok(), "mapping backend parser should accept UI label");
  ok &= expect(hm::stitching::ParseMappingBackend("RANSAC").ok(), "mapping backend parser should accept UI label");
  ok &= expect(
      !hm::stitching::ParseMappingBackend("unknown").ok(), "mapping backend parser should reject unknown choices");

  fs::path three_point_affine_dir = root / "three-point-affine";
  fs::create_directories(three_point_affine_dir);
  const std::vector<hm::stitching::FeatureMatch> three_point_matches = {
      {{10.0f, 10.0f}, {4.0f, 14.0f}, 0.9f},
      {{70.0f, 12.0f}, {64.0f, 16.0f}, 0.9f},
      {{12.0f, 62.0f}, {6.0f, 66.0f}, 0.9f},
  };
  auto three_point_affine = hm::stitching::CreateOpenCvMappingFiles(
      three_point_affine_dir, left, right, three_point_matches, hm::stitching::MappingBackend::kOpenCvAffineRansac);
  ok &= expect(three_point_affine.ok(), "affine RANSAC mapping should accept its three-point minimum");
  ok &= expect(
      !hm::stitching::CreateOpenCvMappingFiles(
           root / "three-point-magsac", left, right, three_point_matches, hm::stitching::MappingBackend::kOpenCvMagsac)
           .ok(),
      "MAGSAC mapping should reject fewer than four control points");

  fs::path rotated_dir = root / "rotated";
  fs::create_directories(rotated_dir);
  std::vector<hm::stitching::FeatureMatch> rotated_matches;
  constexpr double kPi = 3.14159265358979323846;
  const double radians = 12.0 * kPi / 180.0;
  const double c = std::cos(radians);
  const double s = std::sin(radians);
  for (int y = 10; y < 75; y += 10) {
    for (int x = 10; x < 95; x += 10) {
      const double left_x = c * x - s * y + 14.0;
      const double left_y = s * x + c * y - 6.0;
      if (left_x >= 0.0 && left_y >= 0.0 && left_x < left.cols && left_y < left.rows) {
        rotated_matches.push_back(
            {{static_cast<float>(left_x), static_cast<float>(left_y)},
             {static_cast<float>(x), static_cast<float>(y)},
             0.9f});
      }
    }
  }
  auto rotated = hm::stitching::CreateOpenCvMappingFiles(
      rotated_dir, left, right, rotated_matches, hm::stitching::MappingBackend::kOpenCvAffineRansac);
  ok &= expect(rotated.ok(), "rotated affine mapping should generate artifacts");
  if (rotated.ok()) {
    ok &= expect(
        seam_only_selects_valid_remaps(rotated_dir), "OpenCV validity seam should not select unmapped rotated corners");
  }

  fs::path tall_dir = root / "tall-width-cap";
  fs::create_directories(tall_dir);
  std::vector<hm::stitching::FeatureMatch> tall_matches;
  for (int y = 10; y < 75; y += 16) {
    for (int x = 10; x < 95; x += 16) {
      tall_matches.push_back(
          {{static_cast<float>(x), static_cast<float>(y)},
           {static_cast<float>(x - 30), static_cast<float>(y - 160)},
           0.9f});
    }
  }
  auto tall = hm::stitching::CreateOpenCvMappingFiles(
      tall_dir, left, right, tall_matches, hm::stitching::MappingBackend::kOpenCvAffineRansac, std::nullopt, 80);
  ok &= expect(tall.ok(), "OpenCV width cap should allow tall canvas generation");
  if (tall.ok()) {
    ok &= expect(tall->canvas_width == 80, "OpenCV width cap must constrain width");
    ok &= expect(tall->canvas_height > 80, "OpenCV width cap must not act as a longest-side cap");
  }

  fs::path rounding_dir = root / "width-cap-rounding";
  fs::create_directories(rounding_dir);
  const std::vector<hm::stitching::FeatureMatch> rounding_matches = {
      {{2005.0f, 0.0f}, {0.0f, 0.0f}, 0.9f},
      {{2055.0f, 0.0f}, {50.0f, 0.0f}, 0.9f},
      {{2005.0f, 50.0f}, {0.0f, 50.0f}, 0.9f},
      {{2055.0f, 50.0f}, {50.0f, 50.0f}, 0.9f},
  };
  auto rounding = hm::stitching::CreateOpenCvMappingFiles(
      rounding_dir,
      left,
      right,
      rounding_matches,
      hm::stitching::MappingBackend::kOpenCvAffineRansac,
      std::nullopt,
      1536);
  ok &= expect(rounding.ok(), "OpenCV width cap rounding fixture should generate artifacts");
  if (rounding.ok()) {
    ok &= expect(rounding->canvas_width == 1536, "floating-point rounding must not exceed the OpenCV width cap");
  }

  fs::remove_all(root);
  return ok ? 0 : 1;
}
