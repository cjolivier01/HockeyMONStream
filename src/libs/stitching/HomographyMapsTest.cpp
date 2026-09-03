#include "hstream/src/libs/stitching/HomographyMaps.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

#include <opencv2/calib3d.hpp>
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
  const auto& general_panini_parameters =
      hm::stitching::StitchProjectionParameters(hm::stitching::StitchProjection::kGeneralPanini);
  ok &= expect(
      general_panini_parameters.size() == 3 && std::string(general_panini_parameters[0].name) == "Cmpr" &&
          general_panini_parameters[0].minimum == 0.0 && general_panini_parameters[0].maximum == 150.0 &&
          general_panini_parameters[0].default_value == 100.0 &&
          std::string(general_panini_parameters[1].name) == "Tops" && general_panini_parameters[1].minimum == -100.0 &&
          general_panini_parameters[1].maximum == 100.0 && std::string(general_panini_parameters[2].name) == "Bots",
      "General Panini metadata must match libpano's Cmpr,Tops,Bots contract");
  ok &= expect(
      hm::stitching::StitchProjectionParameters(hm::stitching::StitchProjection::kAlbersEqualAreaConic).size() == 2 &&
          hm::stitching::StitchProjectionParameters(hm::stitching::StitchProjection::kBiplane).size() == 2 &&
          hm::stitching::StitchProjectionParameters(hm::stitching::StitchProjection::kTriplane).size() == 1 &&
          hm::stitching::StitchProjectionParameters(hm::stitching::StitchProjection::kPanini).empty() &&
          hm::stitching::StitchProjectionParameters(hm::stitching::StitchProjection::kEquirectangularPanini).empty(),
      "only projections with libpano parameters must advertise adjustable controls");
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

  fs::path calibrated_dir = root / "calibrated";
  fs::create_directories(calibrated_dir);
  hm::stitching::FisheyeLensCalibration lens;
  lens.resolution = left.size();
  lens.fx = 80.0;
  lens.fy = 80.0;
  lens.cx = 50.0;
  lens.cy = 40.0;
  lens.distortion = {0.05, -0.005, 0.0005, -0.00005};
  auto calibrated = hm::stitching::CreateOpenCvMappingFiles(
      calibrated_dir,
      left,
      right,
      matches,
      hm::stitching::MappingBackend::kOpenCvMagsac,
      std::nullopt,
      std::nullopt,
      hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
  ok &= expect(calibrated.ok(), "calibrated AKAZE mapping should generate composed KB4 remaps");
  if (calibrated.ok()) {
    cv::Mat calibrated_left_x =
        cv::imread((calibrated_dir / "mapping_0000_x.tif").string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    cv::Mat calibrated_left_y =
        cv::imread((calibrated_dir / "mapping_0000_y.tif").string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    ok &= expect(
        !calibrated_left_x.empty() && !calibrated_left_y.empty() && calibrated_left_x.at<uint16_t>(0, 0) > 0 &&
            calibrated_left_y.at<uint16_t>(0, 0) > 0,
        "calibrated output coordinates must be distorted back into original fisheye source pixels");
    std::vector<cv::Point2d> normalized = {
        {(10.0 - lens.cx) / lens.fx, (10.0 - lens.cy) / lens.fy},
    };
    std::vector<cv::Point2d> opencv_distorted;
    const cv::Matx33d camera_matrix(lens.fx, 0.0, lens.cx, 0.0, lens.fy, lens.cy, 0.0, 0.0, 1.0);
    const cv::Vec4d distortion(lens.distortion[0], lens.distortion[1], lens.distortion[2], lens.distortion[3]);
    cv::fisheye::distortPoints(normalized, opencv_distorted, camera_matrix, distortion);
    ok &= expect(
        calibrated_left_x.at<uint16_t>(10, 10) == static_cast<uint16_t>(std::lround(opencv_distorted[0].x)) &&
            calibrated_left_y.at<uint16_t>(10, 10) == static_cast<uint16_t>(std::lround(opencv_distorted[0].y)),
        "KB4 remap composition must numerically match OpenCV fisheye distortion");
  }

  fs::path calibrated_projective_dir = root / "calibrated-projective";
  fs::create_directories(calibrated_projective_dir);
  const cv::Matx33d expected_projective(1.0, 0.025, 7.0, 0.015, 1.0, -2.0, 0.001, -0.0004, 1.0);
  std::vector<hm::stitching::FeatureMatch> calibrated_projective_matches;
  for (int y = 8; y <= 72; y += 16) {
    for (int x = 8; x <= 92; x += 14) {
      const cv::Vec3d transformed = expected_projective * cv::Vec3d(x, y, 1.0);
      calibrated_projective_matches.push_back(
          {{static_cast<float>(transformed[0] / transformed[2]), static_cast<float>(transformed[1] / transformed[2])},
           {static_cast<float>(x), static_cast<float>(y)},
           0.9f});
    }
  }
  auto calibrated_projective = hm::stitching::CreateOpenCvMappingFiles(
      calibrated_projective_dir,
      left,
      right,
      calibrated_projective_matches,
      hm::stitching::MappingBackend::kOpenCvMagsac,
      std::nullopt,
      std::nullopt,
      hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
  ok &= expect(calibrated_projective.ok(), "calibrated MAGSAC must fit genuinely projective rectified geometry");
  if (calibrated_projective.ok()) {
    const auto& fitted = calibrated_projective->right_to_left_homography;
    ok &= expect(
        std::abs(fitted[6] / fitted[8] - expected_projective(2, 0)) < 1e-4 &&
            std::abs(fitted[7] / fitted[8] - expected_projective(2, 1)) < 1e-4,
        "calibrated MAGSAC must preserve projective terms instead of silently fitting an affine transform");
  }

  fs::path alternate_hypothesis_dir = root / "alternate-projective-hypothesis";
  fs::create_directories(alternate_hypothesis_dir);
  std::vector<hm::stitching::FeatureMatch> alternate_hypothesis_matches;
  const cv::Matx33d pole_transform(0.5, 0.0, 0.0, 0.0, 0.5, 0.0, -1.0 / 80.0, 0.0, 1.0);
  for (const int y : {5, 10, 15, 20}) {
    for (const int x : {10, 20, 30, 40, 50}) {
      const cv::Vec3d transformed = pole_transform * cv::Vec3d(x, y, 1.0);
      alternate_hypothesis_matches.push_back(
          {{static_cast<float>(transformed[0] / transformed[2]), static_cast<float>(transformed[1] / transformed[2])},
           {static_cast<float>(x), static_cast<float>(y)},
           0.9f});
    }
  }
  for (const int y : {40, 50, 60, 70}) {
    for (const int x : {10, 35, 60, 85}) {
      alternate_hypothesis_matches.push_back(
          {{static_cast<float>(x + 5), static_cast<float>(y - 3)},
           {static_cast<float>(x), static_cast<float>(y)},
           0.8f});
    }
  }
  auto alternate_hypothesis = hm::stitching::CreateOpenCvMappingFiles(
      alternate_hypothesis_dir,
      left,
      right,
      alternate_hypothesis_matches,
      hm::stitching::MappingBackend::kOpenCvMagsac,
      std::nullopt,
      std::nullopt,
      hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
  ok &= expect(
      alternate_hypothesis.ok() && alternate_hypothesis->inlier_count == 16 &&
          std::abs(alternate_hypothesis->right_to_left_homography[2] - 5.0) < 0.25,
      "calibrated MAGSAC must recover a valid secondary projective consensus after rejecting a dominant pole");

  fs::path affine_dir = root / "affine";
  fs::create_directories(affine_dir);
  auto affine = hm::stitching::CreateOpenCvMappingFiles(
      affine_dir, left, right, matches, hm::stitching::MappingBackend::kOpenCvAffineRansac, 64);
  ok &= expect(affine.ok(), "affine RANSAC mapping should generate artifacts");
  if (affine.ok()) {
    ok &= expect(
        std::max(affine->canvas_width, affine->canvas_height) <= 64 && affine->max_canvas_dimension_applied &&
            !affine->max_output_width_applied &&
            affine->source_canvas_width > static_cast<size_t>(affine->canvas_width),
        "max canvas dimension and its provenance state should be honored");
    ok &= expect(affine->inlier_mask.size() == matches.size(), "affine inlier mask should cover input matches");
    cv::Mat scaled_left_x =
        cv::imread((affine_dir / "mapping_0000_x.tif").string(), cv::IMREAD_ANYDEPTH | cv::IMREAD_GRAYSCALE);
    ok &= expect(
        !scaled_left_x.empty() && scaled_left_x.at<uint16_t>(0, scaled_left_x.cols - 1) > 90,
        "scaled left remap should address the full source width");
  }

  ok &= expect(
      hm::stitching::ParseMappingBackend("opencv_magsac").ok(), "mapping backend parser should accept underscores");
  auto default_backend = hm::stitching::ParseMappingBackend("");
  ok &= expect(
      default_backend.ok() && *default_backend == hm::stitching::MappingBackend::kOpenCvMagsac,
      "an omitted mapping backend should default to MAGSAC");
  ok &= expect(hm::stitching::ParseMappingBackend("MAGSAC++").ok(), "mapping backend parser should accept UI label");
  ok &= expect(hm::stitching::ParseMappingBackend("RANSAC").ok(), "mapping backend parser should accept UI label");
  ok &= expect(
      !hm::stitching::ParseMappingBackend("unknown").ok(), "mapping backend parser should reject unknown choices");
  const auto& projections = hm::stitching::SupportedStitchProjections();
  ok &= expect(projections.size() == 22, "all Hugin projection choices must be exposed");
  constexpr std::array<const char*, 22> expected_projection_names = {
      "rectilinear",
      "cylindrical",
      "equirectangular",
      "full-frame-fisheye",
      "stereographic",
      "mercator",
      "transverse-mercator",
      "sinusoidal",
      "lambert-cylindrical-equal-area",
      "lambert-azimuthal-equal-area",
      "albers-equal-area-conic",
      "miller-cylindrical",
      "panini",
      "architectural",
      "orthographic",
      "equisolid",
      "equirectangular-panini",
      "biplane",
      "triplane",
      "general-panini",
      "thoby",
      "hammer-aitoff",
  };
  for (size_t index = 0; index < projections.size(); ++index) {
    const auto parsed = hm::stitching::ParseStitchProjection(projections[index].name);
    ok &= expect(
        parsed.ok() && *parsed == projections[index].projection &&
            std::string(projections[index].name) == expected_projection_names[index] &&
            projections[index].hugin_projection == static_cast<int>(index),
        "projection names must match their explicit Hugin identifiers and round-trip");
  }
  ok &= expect(
      hm::stitching::ParseStitchProjection("panini_general").ok(),
      "projection parser should accept the General Panini alias");
  ok &=
      expect(!hm::stitching::ParseStitchProjection("unknown").ok(), "projection parser should reject unknown choices");
  ok &= expect(
      hm::stitching::ValidateMappingBackendProjection(
          hm::stitching::MappingBackend::kNona, hm::stitching::StitchProjection::kGeneralPanini)
          .ok(),
      "Nona must accept General Panini");
  ok &= expect(
      !hm::stitching::ValidateMappingBackendProjection(
           hm::stitching::MappingBackend::kOpenCvMagsac, hm::stitching::StitchProjection::kGeneralPanini)
              .ok() &&
          hm::stitching::ValidateMappingBackendProjection(
              hm::stitching::MappingBackend::kOpenCvAffineRansac, hm::stitching::StitchProjection::kRectilinear)
              .ok(),
      "OpenCV mapping backends must reject non-rectilinear projections");

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

  fs::path clustered_six_calibrated_dir = root / "clustered-six-calibrated";
  fs::create_directories(clustered_six_calibrated_dir);
  std::vector<hm::stitching::FeatureMatch> clustered_six_calibrated_matches;
  for (const int y : {30, 32}) {
    for (const int x : {40, 42, 44}) {
      clustered_six_calibrated_matches.push_back(
          {{static_cast<float>(x + 12), static_cast<float>(y - 4)},
           {static_cast<float>(x), static_cast<float>(y)},
           0.9f});
    }
  }
  auto clustered_six_calibrated = hm::stitching::CreateOpenCvMappingFiles(
      clustered_six_calibrated_dir,
      left,
      right,
      clustered_six_calibrated_matches,
      hm::stitching::MappingBackend::kOpenCvMagsac,
      std::nullopt,
      std::nullopt,
      hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
  ok &= expect(
      !clustered_six_calibrated.ok() &&
          std::string(clustered_six_calibrated.status().message()).find("inlier coverage") != std::string::npos,
      "calibrated MAGSAC must reject a clustered six-point set despite meeting the AKAZE count minimum");

  const auto calibrated_boundary_matches = [](size_t total, size_t translation_inliers) {
    const std::array<cv::Point2f, 10> kSpread = {
        cv::Point2f{5.0f, 8.0f},
        cv::Point2f{30.0f, 8.0f},
        cv::Point2f{60.0f, 8.0f},
        cv::Point2f{90.0f, 8.0f},
        cv::Point2f{5.0f, 70.0f},
        cv::Point2f{30.0f, 70.0f},
        cv::Point2f{60.0f, 70.0f},
        cv::Point2f{90.0f, 70.0f},
        cv::Point2f{20.0f, 38.0f},
        cv::Point2f{75.0f, 42.0f},
    };
    std::vector<hm::stitching::FeatureMatch> result;
    for (size_t index = 0; index < translation_inliers; ++index) {
      const cv::Point2f point = kSpread[index];
      result.push_back({{point.x + 5.0f, point.y - 3.0f}, point, 0.9f});
    }
    for (size_t index = translation_inliers; index < total; ++index) {
      const float right_x = static_cast<float>((index * 29 + 13) % 93);
      const float right_y = static_cast<float>((index * 47 + 9) % 73);
      result.push_back(
          {{static_cast<float>((index * 61 + 7) % 97), static_cast<float>((index * 31 + 17) % 79)},
           {right_x, right_y},
           0.2f});
    }
    return result;
  };
  for (const auto [total, translation_inliers] :
       {std::pair<size_t, size_t>{15, 8}, std::pair<size_t, size_t>{16, 8}, std::pair<size_t, size_t>{20, 10}}) {
    const fs::path boundary_dir = root / ("calibrated-boundary-" + std::to_string(total));
    fs::create_directories(boundary_dir);
    const auto boundary = hm::stitching::CreateOpenCvMappingFiles(
        boundary_dir,
        left,
        right,
        calibrated_boundary_matches(total, translation_inliers),
        hm::stitching::MappingBackend::kOpenCvMagsac,
        std::nullopt,
        std::nullopt,
        hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
    ok &=
        expect(boundary.ok(), "calibrated MAGSAC consensus must remain continuous across the 15/16/20-match boundary");
  }

  fs::path low_consensus_dir = root / "low-consensus";
  fs::create_directories(low_consensus_dir);
  std::vector<hm::stitching::FeatureMatch> low_consensus_matches = {
      {{17.0f, 6.0f}, {5.0f, 10.0f}, 0.9f},
      {{87.0f, 6.0f}, {75.0f, 10.0f}, 0.9f},
      {{17.0f, 66.0f}, {5.0f, 70.0f}, 0.9f},
      {{87.0f, 66.0f}, {75.0f, 70.0f}, 0.9f},
  };
  for (int i = 0; i < 12; ++i) {
    low_consensus_matches.push_back(
        {{static_cast<float>((i * 37 + 11) % 97), static_cast<float>((i * 53 + 7) % 79)},
         {static_cast<float>(15 + (i % 4) * 20), static_cast<float>(18 + (i / 4) * 22)},
         0.2f});
  }
  auto low_consensus = hm::stitching::CreateOpenCvMappingFiles(
      low_consensus_dir, left, right, low_consensus_matches, hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(
      !low_consensus.ok() &&
          std::string(low_consensus.status().message()).find("insufficient consensus") != std::string::npos,
      "MAGSAC mapping should reject a 16-point set supported by only four inliers");
  fs::path calibrated_low_consensus_dir = root / "calibrated-low-consensus";
  fs::create_directories(calibrated_low_consensus_dir);
  auto calibrated_low_consensus = hm::stitching::CreateOpenCvMappingFiles(
      calibrated_low_consensus_dir,
      left,
      right,
      low_consensus_matches,
      hm::stitching::MappingBackend::kOpenCvMagsac,
      std::nullopt,
      std::nullopt,
      hm::stitching::AkazeMatchingCalibration{.left = lens, .right = lens});
  ok &= expect(
      !calibrated_low_consensus.ok() &&
          std::string(calibrated_low_consensus.status().message()).find("insufficient consensus") != std::string::npos,
      "calibrated MAGSAC must retain the robust mapping-consensus threshold");

  fs::path clustered_consensus_dir = root / "clustered-consensus";
  fs::create_directories(clustered_consensus_dir);
  std::vector<hm::stitching::FeatureMatch> clustered_consensus_matches;
  for (int y = 30; y < 38; y += 2) {
    for (int x = 40; x < 48; x += 2) {
      clustered_consensus_matches.push_back(
          {{static_cast<float>(x + 12), static_cast<float>(y - 4)},
           {static_cast<float>(x), static_cast<float>(y)},
           0.9f});
    }
  }
  auto clustered_consensus = hm::stitching::CreateOpenCvMappingFiles(
      clustered_consensus_dir, left, right, clustered_consensus_matches, hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(
      !clustered_consensus.ok() &&
          std::string(clustered_consensus.status().message()).find("inlier coverage") != std::string::npos,
      "MAGSAC mapping should reject inliers confined to a small source-image region");

  fs::path collapsed_destination_dir = root / "collapsed-destination";
  fs::create_directories(collapsed_destination_dir);
  std::vector<hm::stitching::FeatureMatch> collapsed_destination_matches;
  for (int y = 10; y <= 70; y += 20) {
    for (int x = 10; x <= 70; x += 20) {
      collapsed_destination_matches.push_back(
          {{static_cast<float>(40.0 + x * 0.08), static_cast<float>(30.0 + y * 0.08)},
           {static_cast<float>(x), static_cast<float>(y)},
           0.9f});
    }
  }
  auto collapsed_destination = hm::stitching::CreateOpenCvMappingFiles(
      collapsed_destination_dir,
      left,
      right,
      collapsed_destination_matches,
      hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(
      !collapsed_destination.ok() &&
          std::string(collapsed_destination.status().message()).find("left/destination") != std::string::npos,
      "MAGSAC mapping should reject a destination consensus collapsed into a small repeated-image patch");

  fs::path projective_pole_dir = root / "projective-pole";
  fs::create_directories(projective_pole_dir);
  std::vector<hm::stitching::FeatureMatch> projective_pole_matches;
  for (const float x : {4.0f, 12.0f, 20.0f, 28.0f}) {
    for (const float y : {4.0f, 12.0f, 20.0f}) {
      const float denominator = 1.0f - x / 50.0f;
      projective_pole_matches.push_back({{x / denominator, y / denominator}, {x, y}, 0.9f});
    }
  }
  auto projective_pole = hm::stitching::CreateOpenCvMappingFiles(
      projective_pole_dir, left, right, projective_pole_matches, hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(
      !projective_pole.ok() &&
          std::string(projective_pole.status().message()).find("projective pole") != std::string::npos,
      "MAGSAC mapping should reject a fitted homography whose projective pole crosses the source image");

  fs::path near_projective_pole_dir = root / "near-projective-pole";
  fs::create_directories(near_projective_pole_dir);
  std::vector<hm::stitching::FeatureMatch> near_projective_pole_matches;
  for (const float x : {4.0f, 12.0f, 20.0f, 28.0f}) {
    for (const float y : {4.0f, 12.0f, 20.0f}) {
      const float denominator = 1.0f - x / 99.1f;
      near_projective_pole_matches.push_back({{x / denominator, y / denominator}, {x, y}, 0.9f});
    }
  }
  auto near_projective_pole = hm::stitching::CreateOpenCvMappingFiles(
      near_projective_pole_dir,
      left,
      right,
      near_projective_pole_matches,
      hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(
      !near_projective_pole.ok() && near_projective_pole.status().code() == absl::StatusCode::kFailedPrecondition,
      "MAGSAC mapping should reject near-pole canvas extents as a retryable candidate failure before integer conversion");

  fs::path skewed_projective_dir = root / "skewed-projective";
  fs::create_directories(skewed_projective_dir);
  const std::vector<hm::stitching::FeatureMatch> skewed_projective_matches = {
      {{47.9864197f, 165.182816f}, {0.0f, 0.0f}, 0.9f},
      {{132.504471f, 177.040726f}, {99.0f, 0.0f}, 0.9f},
      {{262.785645f, 359.930878f}, {99.0f, 79.0f}, 0.9f},
      {{347.189575f, 823.083191f}, {0.0f, 79.0f}, 0.9f},
  };
  auto skewed_projective = hm::stitching::CreateOpenCvMappingFiles(
      skewed_projective_dir, left, right, skewed_projective_matches, hm::stitching::MappingBackend::kOpenCvMagsac);
  ok &= expect(skewed_projective.ok(), "a skewed projective mapping should generate bounded remap artifacts");
  if (skewed_projective.ok()) {
    const cv::Mat skewed_x = cv::imread((skewed_projective_dir / "mapping_0001_x.tif").string(), cv::IMREAD_ANYDEPTH);
    const cv::Mat skewed_y = cv::imread((skewed_projective_dir / "mapping_0001_y.tif").string(), cv::IMREAD_ANYDEPTH);
    const TiffPlacement placement = read_tiff_placement(skewed_projective_dir / "mapping_0001.tif");
    const int exterior_x = 232 - placement.x;
    const int exterior_y = 191 - placement.y;
    const auto& h = skewed_projective->right_to_left_homography;
    const cv::Matx33d left_to_right = cv::Matx33d(h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]).inv();
    const cv::Vec3d projected = left_to_right * cv::Vec3d(232.0, 191.0, 1.0);
    const double projected_x = projected[0] / projected[2];
    const double projected_y = projected[1] / projected[2];
    ok &= expect(
        !skewed_x.empty() && !skewed_y.empty() && exterior_x >= 0 && exterior_y >= 0 && exterior_x < skewed_x.cols &&
            exterior_y < skewed_x.rows &&
            (projected_x < -0.5 || projected_x >= right.cols - 0.5 || projected_y < -0.5 ||
             projected_y >= right.rows - 0.5) &&
            !remap_valid_at(skewed_x, skewed_y, exterior_x, exterior_y),
        "a pole-adjacent point inside the TIFF AABB but outside the transformed image polygon must remain unmapped");
  }

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
    ok &= expect(
        tall->canvas_width == 80 && tall->max_output_width_applied && !tall->max_canvas_dimension_applied,
        "OpenCV width cap must constrain width and record that it was applied");
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
