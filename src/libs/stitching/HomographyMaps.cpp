#include "hstream/src/libs/stitching/HomographyMaps.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <locale>
#include <string>
#include <vector>

#include <opencv2/calib3d.hpp>
#include <opencv2/core/version.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <tiffio.h>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

namespace fs = std::filesystem;

constexpr uint16_t kUnmapped = std::numeric_limits<uint16_t>::max();
constexpr double kMagsacReprojectionThreshold = 3.0;
constexpr double kAffineRansacReprojectionThreshold = 10.0;
constexpr double kRansacConfidence = 0.999;
constexpr int kRansacMaxIterations = 10000;
constexpr double kProjectivePoleEpsilon = 1e-9;
constexpr size_t kRobustConsensusMatchCount = 16;
constexpr size_t kMinimumRobustMagsacInliers = 8;
constexpr double kMinimumMagsacInlierRatio = 0.5;
constexpr double kMinimumMagsacSourceSpanRatio = 0.1;
constexpr double kMinimumMagsacSourceAreaRatio = 0.01;

absl::Status validate_image(const cv::Mat& image, const char* name) {
  if (image.empty() || image.type() != CV_8UC3)
    return absl::InvalidArgumentError(std::string(name) + " image must be non-empty CV_8UC3");
  if (image.cols >= kUnmapped || image.rows >= kUnmapped)
    return absl::ResourceExhaustedError(std::string(name) + " image is too large for uint16 remap coordinates");
  return absl::OkStatus();
}

absl::Status validate_output_size(double width, double height) {
  if (!std::isfinite(width) || !std::isfinite(height) || width <= 0.0 || height <= 0.0 ||
      width >= static_cast<double>(kUnmapped) || height >= static_cast<double>(kUnmapped) ||
      width > static_cast<double>(std::numeric_limits<int>::max()) ||
      height > static_cast<double>(std::numeric_limits<int>::max())) {
    return absl::FailedPreconditionError("OpenCV mapping candidate canvas exceeds uint16 remap limits");
  }
  constexpr int64_t kMaxPixels = 128LL * 1024LL * 1024LL;
  if (width > static_cast<double>(kMaxPixels) / height)
    return absl::FailedPreconditionError("OpenCV mapping candidate canvas exceeds decoded-image safety limits");
  return absl::OkStatus();
}

absl::Status write_u16_tiff(const fs::path& path, const cv::Mat& image) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (tif == nullptr)
    return absl::InternalError("Unable to open TIFF for writing: " + path.string());
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(image.cols));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(image.rows));
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 1);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(tif, TIFFTAG_SAMPLEFORMAT, SAMPLEFORMAT_UINT);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_MINISBLACK);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, std::min<uint32_t>(static_cast<uint32_t>(image.rows), 64));
  for (uint32_t row = 0; row < static_cast<uint32_t>(image.rows); ++row) {
    if (TIFFWriteScanline(tif, const_cast<uint16_t*>(image.ptr<uint16_t>(row)), row, 0) < 0) {
      TIFFClose(tif);
      return absl::InternalError("Unable to write remap TIFF scanline: " + path.string());
    }
  }
  TIFFClose(tif);
  return absl::OkStatus();
}

bool finite_matrix(const cv::Mat& matrix) {
  if (matrix.empty())
    return false;
  cv::Mat as64;
  matrix.convertTo(as64, CV_64F);
  for (int y = 0; y < as64.rows; ++y) {
    const double* row = as64.ptr<double>(y);
    for (int x = 0; x < as64.cols; ++x) {
      if (!std::isfinite(row[x]))
        return false;
    }
  }
  return true;
}

absl::Status validate_magsac_consensus(
    const std::vector<FeatureMatch>& matches,
    const cv::Mat& inliers,
    size_t inlier_count,
    const cv::Mat& left_image,
    const cv::Mat& right_image,
    double minimum_inlier_ratio = kMinimumMagsacInlierRatio) {
  if (matches.size() < kRobustConsensusMatchCount)
    return absl::OkStatus();

  const size_t ratio_inliers =
      static_cast<size_t>(std::ceil(minimum_inlier_ratio * static_cast<double>(matches.size())));
  const size_t required_inliers = std::max(kMinimumRobustMagsacInliers, ratio_inliers);
  if (inlier_count < required_inliers) {
    return absl::FailedPreconditionError(
        "OpenCV MAGSAC stitching transform has insufficient consensus: " + std::to_string(inlier_count) +
        " inliers from " + std::to_string(matches.size()) + " control points; at least " +
        std::to_string(required_inliers) + " are required");
  }

  const cv::Mat flat_inliers = inliers.reshape(1, 1);
  const unsigned char* mask = flat_inliers.ptr<unsigned char>();
  std::vector<cv::Point2f> left_inliers;
  std::vector<cv::Point2f> right_inliers;
  left_inliers.reserve(inlier_count);
  right_inliers.reserve(inlier_count);
  for (size_t i = 0; i < matches.size(); ++i) {
    if (mask[i]) {
      left_inliers.push_back(matches[i].left);
      right_inliers.push_back(matches[i].right);
    }
  }

  const auto validate_coverage = [](const std::vector<cv::Point2f>& points, const cv::Mat& image, const char* label) {
    float minimum_x = points.front().x;
    float maximum_x = minimum_x;
    float minimum_y = points.front().y;
    float maximum_y = minimum_y;
    for (const cv::Point2f& point : points) {
      minimum_x = std::min(minimum_x, point.x);
      maximum_x = std::max(maximum_x, point.x);
      minimum_y = std::min(minimum_y, point.y);
      maximum_y = std::max(maximum_y, point.y);
    }
    std::vector<cv::Point2f> hull;
    cv::convexHull(points, hull);
    const double covered_area = std::abs(cv::contourArea(hull));
    const double image_area = static_cast<double>(image.cols) * image.rows;
    if (maximum_x - minimum_x < kMinimumMagsacSourceSpanRatio * image.cols ||
        maximum_y - minimum_y < kMinimumMagsacSourceSpanRatio * image.rows ||
        covered_area < kMinimumMagsacSourceAreaRatio * image_area) {
      return absl::FailedPreconditionError(
          std::string("OpenCV MAGSAC stitching transform has insufficient inlier coverage across the ") + label +
          " image");
    }
    return absl::OkStatus();
  };
  absl::Status coverage = validate_coverage(right_inliers, right_image, "right/source");
  if (!coverage.ok())
    return coverage;
  coverage = validate_coverage(left_inliers, left_image, "left/destination");
  if (!coverage.ok())
    return coverage;
  return absl::OkStatus();
}

absl::Status write_rgba_tiff_with_placement(
    const fs::path& path,
    const cv::Mat& bgr,
    const cv::Mat& x_map,
    const cv::Mat& y_map,
    int x_px,
    int y_px) {
  TIFF* tif = TIFFOpen(path.c_str(), "w");
  if (tif == nullptr)
    return absl::InternalError("Unable to open TIFF for writing: " + path.string());
  constexpr float kResolution = 1.0f;
  TIFFSetField(tif, TIFFTAG_IMAGEWIDTH, static_cast<uint32_t>(bgr.cols));
  TIFFSetField(tif, TIFFTAG_IMAGELENGTH, static_cast<uint32_t>(bgr.rows));
  TIFFSetField(tif, TIFFTAG_SAMPLESPERPIXEL, 4);
  TIFFSetField(tif, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(tif, TIFFTAG_ORIENTATION, ORIENTATION_TOPLEFT);
  TIFFSetField(tif, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(tif, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  uint16_t extra_sample = EXTRASAMPLE_UNASSALPHA;
  TIFFSetField(tif, TIFFTAG_EXTRASAMPLES, 1, &extra_sample);
  TIFFSetField(tif, TIFFTAG_ROWSPERSTRIP, std::min<uint32_t>(static_cast<uint32_t>(bgr.rows), 64));
  TIFFSetField(tif, TIFFTAG_XRESOLUTION, kResolution);
  TIFFSetField(tif, TIFFTAG_YRESOLUTION, kResolution);
  TIFFSetField(tif, TIFFTAG_XPOSITION, static_cast<float>(x_px) / kResolution);
  TIFFSetField(tif, TIFFTAG_YPOSITION, static_cast<float>(y_px) / kResolution);

  std::vector<uint8_t> row(static_cast<size_t>(bgr.cols) * 4);
  for (uint32_t y = 0; y < static_cast<uint32_t>(bgr.rows); ++y) {
    const cv::Vec3b* in = bgr.ptr<cv::Vec3b>(static_cast<int>(y));
    const uint16_t* x_in = x_map.ptr<uint16_t>(static_cast<int>(y));
    const uint16_t* y_in = y_map.ptr<uint16_t>(static_cast<int>(y));
    for (int x = 0; x < bgr.cols; ++x) {
      row[static_cast<size_t>(x) * 4] = in[x][2];
      row[static_cast<size_t>(x) * 4 + 1] = in[x][1];
      row[static_cast<size_t>(x) * 4 + 2] = in[x][0];
      row[static_cast<size_t>(x) * 4 + 3] = (x_in[x] == kUnmapped || y_in[x] == kUnmapped) ? 0 : 255;
    }
    if (TIFFWriteScanline(tif, row.data(), y, 0) < 0) {
      TIFFClose(tif);
      return absl::InternalError("Unable to write RGB TIFF scanline: " + path.string());
    }
  }
  TIFFClose(tif);
  return absl::OkStatus();
}

std::array<cv::Point2d, 4> image_corners(const cv::Mat& image) {
  return {
      cv::Point2d(0.0, 0.0),
      cv::Point2d(image.cols - 1.0, 0.0),
      cv::Point2d(image.cols - 1.0, image.rows - 1.0),
      cv::Point2d(0.0, image.rows - 1.0)};
}

absl::StatusOr<cv::Point2d> transform_point(const cv::Matx33d& matrix, const cv::Point2d& point) {
  const double denominator = matrix(2, 0) * point.x + matrix(2, 1) * point.y + matrix(2, 2);
  if (!std::isfinite(denominator) || std::abs(denominator) < kProjectivePoleEpsilon)
    return absl::FailedPreconditionError("OpenCV stitching transform has a projective pole in the mapping canvas");
  const double x = (matrix(0, 0) * point.x + matrix(0, 1) * point.y + matrix(0, 2)) / denominator;
  const double y = (matrix(1, 0) * point.x + matrix(1, 1) * point.y + matrix(1, 2)) / denominator;
  if (!std::isfinite(x) || !std::isfinite(y))
    return absl::FailedPreconditionError("OpenCV stitching transform produced non-finite coordinates");
  return cv::Point2d{x, y};
}

absl::Status validate_projective_domain(const cv::Matx33d& matrix, const cv::Mat& image) {
  std::optional<bool> negative_denominator;
  for (const cv::Point2d& point : image_corners(image)) {
    const double denominator = matrix(2, 0) * point.x + matrix(2, 1) * point.y + matrix(2, 2);
    if (!std::isfinite(denominator) || std::abs(denominator) < kProjectivePoleEpsilon) {
      return absl::FailedPreconditionError("OpenCV stitching transform has a projective pole in the source image");
    }
    const bool negative = std::signbit(denominator);
    if (negative_denominator.has_value() && negative != *negative_denominator) {
      return absl::FailedPreconditionError("OpenCV stitching transform has a projective pole in the source image");
    }
    negative_denominator = negative;
  }
  return absl::OkStatus();
}

cv::Matx33d to_matx33(const cv::Mat& matrix) {
  cv::Mat as64;
  matrix.convertTo(as64, CV_64F);
  return {
      as64.at<double>(0, 0),
      as64.at<double>(0, 1),
      as64.at<double>(0, 2),
      as64.at<double>(1, 0),
      as64.at<double>(1, 1),
      as64.at<double>(1, 2),
      as64.at<double>(2, 0),
      as64.at<double>(2, 1),
      as64.at<double>(2, 2)};
}

cv::Point2d distort_rectified_point(
    const cv::Point2d& point,
    const FisheyeLensCalibration& calibration,
    int source_w,
    int source_h) {
  const double scale_x = static_cast<double>(source_w) / calibration.resolution.width;
  const double scale_y = static_cast<double>(source_h) / calibration.resolution.height;
  const double fx = calibration.fx * scale_x;
  const double fy = calibration.fy * scale_y;
  const double cx = calibration.cx * scale_x;
  const double cy = calibration.cy * scale_y;
  const double normalized_x = (point.x - cx) / fx;
  const double normalized_y = (point.y - cy) / fy;
  const double radius = std::hypot(normalized_x, normalized_y);
  double radial_scale = 1.0;
  if (radius > 1e-12) {
    const double theta = std::atan(radius);
    const double theta2 = theta * theta;
    const double theta4 = theta2 * theta2;
    const double theta6 = theta4 * theta2;
    const double theta8 = theta4 * theta4;
    const double distorted_theta = theta *
        (1.0 + calibration.distortion[0] * theta2 + calibration.distortion[1] * theta4 +
         calibration.distortion[2] * theta6 + calibration.distortion[3] * theta8);
    radial_scale = distorted_theta / radius;
  }
  return {fx * normalized_x * radial_scale + cx, fy * normalized_y * radial_scale + cy};
}

cv::Point2d source_point(
    const cv::Point2d& rectified,
    const std::optional<FisheyeLensCalibration>& calibration,
    int source_w,
    int source_h) {
  return calibration.has_value() ? distort_rectified_point(rectified, *calibration, source_w, source_h) : rectified;
}

void fill_identity_maps(
    cv::Mat* x_map,
    cv::Mat* y_map,
    double min_x,
    double min_y,
    double scale,
    int x0,
    int y0,
    int source_w,
    int source_h,
    const std::optional<FisheyeLensCalibration>& calibration) {
  for (int y = 0; y < x_map->rows; ++y) {
    uint16_t* x_row = x_map->ptr<uint16_t>(y);
    uint16_t* y_row = y_map->ptr<uint16_t>(y);
    for (int x = 0; x < x_map->cols; ++x) {
      const cv::Point2d source = source_point(
          {min_x + static_cast<double>(x0 + x) / scale, min_y + static_cast<double>(y0 + y) / scale},
          calibration,
          source_w,
          source_h);
      const int source_x = static_cast<int>(std::lround(source.x));
      const int source_y = static_cast<int>(std::lround(source.y));
      if (source_x >= 0 && source_y >= 0 && source_x < source_w && source_y < source_h) {
        x_row[x] = static_cast<uint16_t>(source_x);
        y_row[x] = static_cast<uint16_t>(source_y);
      }
    }
  }
}

void fill_projective_maps(
    cv::Mat* x_map,
    cv::Mat* y_map,
    const cv::Matx33d& canvas_to_right,
    int x0,
    int y0,
    int source_w,
    int source_h,
    const std::optional<FisheyeLensCalibration>& calibration) {
  for (int y = 0; y < x_map->rows; ++y) {
    uint16_t* x_row = x_map->ptr<uint16_t>(y);
    uint16_t* y_row = y_map->ptr<uint16_t>(y);
    for (int x = 0; x < x_map->cols; ++x) {
      const double canvas_x = static_cast<double>(x0 + x);
      const double canvas_y = static_cast<double>(y0 + y);
      const double denominator =
          canvas_to_right(2, 0) * canvas_x + canvas_to_right(2, 1) * canvas_y + canvas_to_right(2, 2);
      if (!std::isfinite(denominator) || std::abs(denominator) < kProjectivePoleEpsilon)
        continue;
      const cv::Point2d rectified = {
          (canvas_to_right(0, 0) * canvas_x + canvas_to_right(0, 1) * canvas_y + canvas_to_right(0, 2)) / denominator,
          (canvas_to_right(1, 0) * canvas_x + canvas_to_right(1, 1) * canvas_y + canvas_to_right(1, 2)) / denominator};
      const cv::Point2d source = source_point(rectified, calibration, source_w, source_h);
      if (!std::isfinite(source.x) || !std::isfinite(source.y) || source.x <= -0.5 || source.y <= -0.5 ||
          source.x >= static_cast<double>(source_w) - 0.5 || source.y >= static_cast<double>(source_h) - 0.5)
        continue;
      const int sx = static_cast<int>(std::lround(source.x));
      const int sy = static_cast<int>(std::lround(source.y));
      if (sx >= 0 && sy >= 0 && sx < source_w && sy < source_h) {
        x_row[x] = static_cast<uint16_t>(sx);
        y_row[x] = static_cast<uint16_t>(sy);
      }
    }
  }
}

absl::Status write_maps(
    const fs::path& directory,
    const std::string& prefix,
    const cv::Mat& bgr,
    const cv::Mat& x_map,
    const cv::Mat& y_map,
    int x_px,
    int y_px) {
  auto status = write_rgba_tiff_with_placement(directory / (prefix + ".tif"), bgr, x_map, y_map, x_px, y_px);
  if (!status.ok())
    return status;
  status = write_u16_tiff(directory / (prefix + "_x.tif"), x_map);
  if (!status.ok())
    return status;
  return write_u16_tiff(directory / (prefix + "_y.tif"), y_map);
}

bool remap_valid_at(const cv::Mat& x_map, const cv::Mat& y_map, int x, int y) {
  return x >= 0 && y >= 0 && x < x_map.cols && y < x_map.rows && x_map.at<uint16_t>(y, x) != kUnmapped &&
      y_map.at<uint16_t>(y, x) != kUnmapped;
}

absl::Status write_validity_seam(
    const fs::path& directory,
    int canvas_width,
    int canvas_height,
    int left_x0,
    int left_y0,
    const cv::Mat& left_x,
    const cv::Mat& left_y,
    int right_x0,
    int right_y0,
    const cv::Mat& right_x,
    const cv::Mat& right_y) {
  cv::Mat seam(canvas_height, canvas_width, CV_8U, cv::Scalar(0));
  const int overlap_start = std::max(left_x0, right_x0);
  const int overlap_end = std::min(left_x0 + left_x.cols, right_x0 + right_x.cols);
  const int seam_x = overlap_end > overlap_start ? overlap_start + (overlap_end - overlap_start) / 2 : canvas_width / 2;
  bool selected_left = false;
  bool selected_right = false;
  for (int y = 0; y < canvas_height; ++y) {
    uint8_t* row = seam.ptr<uint8_t>(y);
    for (int x = 0; x < canvas_width; ++x) {
      const bool left_valid = remap_valid_at(left_x, left_y, x - left_x0, y - left_y0);
      const bool right_valid = remap_valid_at(right_x, right_y, x - right_x0, y - right_y0);
      if (right_valid && (!left_valid || x >= seam_x)) {
        row[x] = 255;
        selected_right = true;
      } else if (left_valid) {
        row[x] = 0;
        selected_left = true;
      }
    }
  }
  if (!selected_left || !selected_right)
    return absl::FailedPreconditionError("OpenCV mapping validity did not produce a usable two-camera seam");
  if (!cv::imwrite((directory / "seam_file.png").string(), seam))
    return absl::InternalError("Unable to write OpenCV validity seam");
  return absl::OkStatus();
}

} // namespace

absl::StatusOr<HomographyMapResult> CreateOpenCvMappingFiles(
    const fs::path& directory,
    const cv::Mat& left_bgr,
    const cv::Mat& right_bgr,
    const std::vector<FeatureMatch>& matches,
    MappingBackend backend,
    const std::optional<size_t>& max_canvas_dimension,
    const std::optional<size_t>& max_output_width,
    const AkazeMatchingCalibration& lens_calibration) {
  auto status = validate_image(left_bgr, "left");
  if (!status.ok())
    return status;
  status = validate_image(right_bgr, "right");
  if (!status.ok())
    return status;
  if (backend == MappingBackend::kNona)
    return absl::InvalidArgumentError("OpenCV mapping helper cannot run the nona backend");

  std::vector<cv::Point2f> left_points;
  std::vector<cv::Point2f> right_points;
  left_points.reserve(matches.size());
  right_points.reserve(matches.size());
  for (const FeatureMatch& match : matches) {
    left_points.push_back(match.left);
    right_points.push_back(match.right);
  }

  cv::Mat inliers;
  cv::Mat right_to_left;
  if (backend == MappingBackend::kOpenCvMagsac) {
    if (matches.size() < 4)
      return absl::FailedPreconditionError("At least four control points are required for OpenCV MAGSAC mapping");
    if (lens_calibration.left.has_value() != lens_calibration.right.has_value())
      return absl::InvalidArgumentError("OpenCV calibrated mapping requires both camera lens profiles");
#if CV_VERSION_MAJOR > 4 || (CV_VERSION_MAJOR == 4 && CV_VERSION_MINOR >= 5)
    constexpr int method = cv::USAC_MAGSAC;
#else
    constexpr int method = cv::RANSAC;
#endif
    // Calibrated AKAZE supplies rectified points. Preserve the selected projective backend here and compose the KB4
    // distortion into the generated source remaps below.
    right_to_left = cv::findHomography(
        right_points,
        left_points,
        method,
        kMagsacReprojectionThreshold,
        inliers,
        kRansacMaxIterations,
        kRansacConfidence);
  } else {
    if (matches.size() < 3)
      return absl::FailedPreconditionError("At least three control points are required for affine RANSAC mapping");
    cv::Mat affine = cv::estimateAffine2D(
        right_points,
        left_points,
        inliers,
        cv::RANSAC,
        kAffineRansacReprojectionThreshold,
        kRansacMaxIterations,
        kRansacConfidence,
        10);
    if (!affine.empty()) {
      right_to_left = cv::Mat::eye(3, 3, CV_64F);
      affine.convertTo(right_to_left(cv::Rect(0, 0, 3, 2)), CV_64F);
    }
  }
  if (right_to_left.empty() || !finite_matrix(right_to_left))
    return absl::FailedPreconditionError("OpenCV failed to estimate a stitching transform");
  size_t inlier_count = 0;
  if (!inliers.empty()) {
    const unsigned char* mask = inliers.ptr<unsigned char>();
    for (int i = 0; i < inliers.rows * inliers.cols; ++i) {
      if (mask[i])
        ++inlier_count;
    }
  }
  const size_t minimum_inliers = backend == MappingBackend::kOpenCvMagsac ? 4 : 3;
  if (inlier_count < minimum_inliers) {
    return absl::FailedPreconditionError(
        "OpenCV stitching transform has too few inlier control points: " + std::to_string(inlier_count));
  }
  if (backend == MappingBackend::kOpenCvMagsac) {
    status = validate_magsac_consensus(matches, inliers, inlier_count, left_bgr, right_bgr);
    if (!status.ok())
      return status;
  }

  cv::Mat right_to_left_64;
  right_to_left.convertTo(right_to_left_64, CV_64F);
  const double determinant = cv::determinant(right_to_left_64);
  if (!std::isfinite(determinant) || std::abs(determinant) < 1e-12)
    return absl::FailedPreconditionError("OpenCV stitching transform is not invertible");
  cv::Mat left_to_right_64 = right_to_left_64.inv();
  if (left_to_right_64.empty() || !finite_matrix(left_to_right_64))
    return absl::FailedPreconditionError("OpenCV inverse stitching transform is invalid");
  cv::Matx33d rtl = to_matx33(right_to_left_64);
  cv::Matx33d ltr = to_matx33(left_to_right_64);
  status = validate_projective_domain(rtl, right_bgr);
  if (!status.ok())
    return status;

  std::vector<cv::Point2d> canvas_points;
  for (const cv::Point2d& p : image_corners(left_bgr))
    canvas_points.push_back(p);
  for (const cv::Point2d& p : image_corners(right_bgr)) {
    auto transformed = transform_point(rtl, p);
    if (!transformed.ok())
      return transformed.status();
    canvas_points.push_back(*transformed);
  }
  double min_x = canvas_points[0].x;
  double min_y = canvas_points[0].y;
  double max_x = canvas_points[0].x;
  double max_y = canvas_points[0].y;
  for (const cv::Point2d& p : canvas_points) {
    min_x = std::min(min_x, p.x);
    min_y = std::min(min_y, p.y);
    max_x = std::max(max_x, p.x);
    max_y = std::max(max_y, p.y);
  }

  double scale = 1.0;
  const double raw_width = std::ceil(max_x - min_x + 1.0);
  const double raw_height = std::ceil(max_y - min_y + 1.0);
  if (!std::isfinite(raw_width) || !std::isfinite(raw_height) || raw_width <= 0.0 || raw_height <= 0.0)
    return absl::FailedPreconditionError("OpenCV mapping canvas dimensions are invalid");
  if (raw_width > static_cast<double>(std::numeric_limits<size_t>::max()) ||
      raw_height > static_cast<double>(std::numeric_limits<size_t>::max())) {
    return absl::ResourceExhaustedError("OpenCV source canvas dimensions exceed size limits");
  }
  double width_scale = 1.0;
  if (max_output_width.has_value() && raw_width > static_cast<double>(*max_output_width))
    width_scale = static_cast<double>(*max_output_width) / raw_width;
  double dimension_scale = 1.0;
  if (max_canvas_dimension.has_value()) {
    const double longest = std::max(raw_width, raw_height);
    if (longest > static_cast<double>(*max_canvas_dimension))
      dimension_scale = static_cast<double>(*max_canvas_dimension) / longest;
  }
  scale = std::min(width_scale, dimension_scale);
  double rounded_width = std::ceil(raw_width * scale);
  double rounded_height = std::ceil(raw_height * scale);
  if (max_output_width.has_value())
    rounded_width = std::min(rounded_width, static_cast<double>(*max_output_width));
  if (max_canvas_dimension.has_value()) {
    rounded_width = std::min(rounded_width, static_cast<double>(*max_canvas_dimension));
    rounded_height = std::min(rounded_height, static_cast<double>(*max_canvas_dimension));
  }
  if (rounded_width > static_cast<double>(std::numeric_limits<int>::max()) ||
      rounded_height > static_cast<double>(std::numeric_limits<int>::max())) {
    return absl::ResourceExhaustedError("OpenCV mapping canvas dimensions exceed integer limits");
  }
  const int canvas_width = static_cast<int>(rounded_width);
  const int canvas_height = static_cast<int>(rounded_height);
  status = validate_output_size(canvas_width, canvas_height);
  if (!status.ok())
    return status;

  const int left_x0 = static_cast<int>(std::floor((0.0 - min_x) * scale));
  const int left_y0 = static_cast<int>(std::floor((0.0 - min_y) * scale));
  const int left_w = std::min(canvas_width - left_x0, static_cast<int>(std::ceil(left_bgr.cols * scale)));
  const int left_h = std::min(canvas_height - left_y0, static_cast<int>(std::ceil(left_bgr.rows * scale)));
  if (left_w <= 0 || left_h <= 0)
    return absl::FailedPreconditionError("OpenCV left mapping is outside the canvas");
  cv::Mat left_preview(left_h, left_w, CV_8UC3, cv::Scalar(0, 0, 0));
  cv::Mat left_x(left_h, left_w, CV_16U, cv::Scalar(kUnmapped));
  cv::Mat left_y(left_h, left_w, CV_16U, cv::Scalar(kUnmapped));
  fill_identity_maps(
      &left_x, &left_y, min_x, min_y, scale, left_x0, left_y0, left_bgr.cols, left_bgr.rows, lens_calibration.left);
  for (int y = 0; y < left_h; ++y) {
    for (int x = 0; x < left_w; ++x) {
      const uint16_t sx = left_x.at<uint16_t>(y, x);
      const uint16_t sy = left_y.at<uint16_t>(y, x);
      if (sx != kUnmapped && sy != kUnmapped)
        left_preview.at<cv::Vec3b>(y, x) = left_bgr.at<cv::Vec3b>(sy, sx);
    }
  }

  std::array<cv::Point2d, 4> right_canvas{};
  for (size_t i = 0; i < right_canvas.size(); ++i) {
    auto transformed = transform_point(rtl, image_corners(right_bgr)[i]);
    if (!transformed.ok())
      return transformed.status();
    const cv::Point2d p = *transformed;
    right_canvas[i] = {(p.x - min_x) * scale, (p.y - min_y) * scale};
  }
  double rmin_x = right_canvas[0].x;
  double rmin_y = right_canvas[0].y;
  double rmax_x = right_canvas[0].x;
  double rmax_y = right_canvas[0].y;
  for (const cv::Point2d& p : right_canvas) {
    rmin_x = std::min(rmin_x, p.x);
    rmin_y = std::min(rmin_y, p.y);
    rmax_x = std::max(rmax_x, p.x);
    rmax_y = std::max(rmax_y, p.y);
  }
  const int right_x0 = std::max(0, static_cast<int>(std::floor(rmin_x)));
  const int right_y0 = std::max(0, static_cast<int>(std::floor(rmin_y)));
  const int right_x1 = std::min(canvas_width, static_cast<int>(std::ceil(rmax_x + 1.0)));
  const int right_y1 = std::min(canvas_height, static_cast<int>(std::ceil(rmax_y + 1.0)));
  if (right_x1 <= right_x0 || right_y1 <= right_y0)
    return absl::FailedPreconditionError("OpenCV right mapping is outside the canvas");
  const int right_w = right_x1 - right_x0;
  const int right_h = right_y1 - right_y0;
  cv::Mat right_x(right_h, right_w, CV_16U, cv::Scalar(kUnmapped));
  cv::Mat right_y(right_h, right_w, CV_16U, cv::Scalar(kUnmapped));
  cv::Mat right_preview(right_h, right_w, CV_8UC3, cv::Scalar(0, 0, 0));
  const cv::Matx33d scale_to_left_space(1.0 / scale, 0.0, min_x, 0.0, 1.0 / scale, min_y, 0.0, 0.0, 1.0);
  const cv::Matx33d canvas_to_right = ltr * scale_to_left_space;
  fill_projective_maps(
      &right_x, &right_y, canvas_to_right, right_x0, right_y0, right_bgr.cols, right_bgr.rows, lens_calibration.right);
  for (int y = 0; y < right_h; ++y) {
    for (int x = 0; x < right_w; ++x) {
      const uint16_t sx = right_x.at<uint16_t>(y, x);
      const uint16_t sy = right_y.at<uint16_t>(y, x);
      if (sx != kUnmapped && sy != kUnmapped)
        right_preview.at<cv::Vec3b>(y, x) = right_bgr.at<cv::Vec3b>(sy, sx);
    }
  }

  status = write_maps(directory, "mapping_0000", left_preview, left_x, left_y, left_x0, left_y0);
  if (!status.ok())
    return status;
  status = write_maps(directory, "mapping_0001", right_preview, right_x, right_y, right_x0, right_y0);
  if (!status.ok())
    return status;
  status = write_validity_seam(
      directory, canvas_width, canvas_height, left_x0, left_y0, left_x, left_y, right_x0, right_y0, right_x, right_y);
  if (!status.ok())
    return status;

  HomographyMapResult result;
  result.source_canvas_width = static_cast<size_t>(raw_width);
  result.source_canvas_height = static_cast<size_t>(raw_height);
  result.canvas_width = canvas_width;
  result.canvas_height = canvas_height;
  result.max_output_width_applied = width_scale <= dimension_scale && width_scale < 1.0;
  result.max_canvas_dimension_applied = dimension_scale <= width_scale && dimension_scale < 1.0;
  result.inlier_count = inlier_count;
  result.right_to_left_homography.reserve(9);
  for (int row = 0; row < 3; ++row)
    for (int col = 0; col < 3; ++col)
      result.right_to_left_homography.push_back(rtl(row, col));
  if (!inliers.empty()) {
    result.inlier_mask.reserve(static_cast<size_t>(inliers.total()));
    for (int i = 0; i < inliers.rows * inliers.cols; ++i)
      result.inlier_mask.push_back(inliers.ptr<unsigned char>()[i] ? 1 : 0);
  }
  return result;
}

} // namespace hm::stitching
