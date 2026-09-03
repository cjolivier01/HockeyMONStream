#include "hstream/src/libs/stitching/FeatureMatcher.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

#include <opencv2/calib3d.hpp>
#include <opencv2/features2d.hpp>
#include <opencv2/imgproc.hpp>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

absl::Status validate_source_image(const cv::Mat& image, const char* side) {
  if (image.empty() || image.channels() != 3 || (image.depth() != CV_8U && image.depth() != CV_16U)) {
    return absl::InvalidArgumentError(std::string(side) + " feature image must be non-empty 8-bit or 16-bit BGR");
  }
  return absl::OkStatus();
}

float bgr_channel_to_unit_float(const cv::Mat& image, int y, int x, int channel) {
  if (image.depth() == CV_16U) {
    return static_cast<float>(image.ptr<cv::Vec3w>(y)[x][channel]) / 65535.0f;
  }
  return static_cast<float>(image.ptr<cv::Vec3b>(y)[x][channel]) / 255.0f;
}

absl::StatusOr<FeaturePairInput> prepare_feature_pair(
    const cv::Mat& left_bgr,
    const cv::Mat& right_bgr,
    int input_channels) {
  auto status = validate_source_image(left_bgr, "Left");
  if (!status.ok())
    return status;
  status = validate_source_image(right_bgr, "Right");
  if (!status.ok())
    return status;
  if (input_channels != 1 && input_channels != 3) {
    return absl::InvalidArgumentError("Feature matcher input channel count must be 1 or 3");
  }

  FeaturePairInput result;
  result.source_sizes[0] = left_bgr.size();
  result.source_sizes[1] = right_bgr.size();
  result.tensor_size = {FeatureMatcher::kInputWidth, FeatureMatcher::kInputHeight};
  const size_t image_plane = static_cast<size_t>(FeatureMatcher::kInputHeight) * FeatureMatcher::kInputWidth;
  result.tensor.assign(2 * static_cast<size_t>(input_channels) * image_plane, 0.0f);
  const cv::Mat* images[] = {&left_bgr, &right_bgr};
  for (int image_index = 0; image_index < 2; ++image_index) {
    const cv::Mat& source = *images[image_index];
    const double scale = std::min(
        static_cast<double>(FeatureMatcher::kInputWidth) / source.cols,
        static_cast<double>(FeatureMatcher::kInputHeight) / source.rows);
    const int width = std::max(32, static_cast<int>(std::round(source.cols * scale)));
    const int height = std::max(32, static_cast<int>(std::round(source.rows * scale)));
    if (width > FeatureMatcher::kInputWidth || height > FeatureMatcher::kInputHeight) {
      return absl::InternalError("Feature resize exceeded its fixed ONNX canvas");
    }
    result.resized_sizes[image_index] = {width, height};
    cv::Mat resized;
    cv::resize(source, resized, {width, height}, 0.0, 0.0, cv::INTER_AREA);
    const size_t image_base = static_cast<size_t>(image_index) * input_channels * image_plane;
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const float blue = bgr_channel_to_unit_float(resized, y, x, 0);
        const float green = bgr_channel_to_unit_float(resized, y, x, 1);
        const float red = bgr_channel_to_unit_float(resized, y, x, 2);
        if (input_channels == 1) {
          result.tensor[image_base + static_cast<size_t>(y) * FeatureMatcher::kInputWidth + x] =
              0.299f * red + 0.587f * green + 0.114f * blue;
          continue;
        }
        result.tensor
            [image_base + static_cast<size_t>(0) * image_plane + static_cast<size_t>(y) * FeatureMatcher::kInputWidth +
             x] = red;
        result.tensor
            [image_base + static_cast<size_t>(1) * image_plane + static_cast<size_t>(y) * FeatureMatcher::kInputWidth +
             x] = green;
        result.tensor
            [image_base + static_cast<size_t>(2) * image_plane + static_cast<size_t>(y) * FeatureMatcher::kInputWidth +
             x] = blue;
      }
    }
  }
  return result;
}

cv::Mat grayscale_u8(const cv::Mat& bgr) {
  cv::Mat gray;
  cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
  if (gray.depth() == CV_16U) {
    cv::Mat converted;
    gray.convertTo(converted, CV_8U, 255.0 / 65535.0);
    return converted;
  }
  return gray;
}

cv::Size aligned_loftr_size(cv::Size source) {
  const double scale = std::min(
      1.0, static_cast<double>(FeatureMatcher::kLoFTRMaximumDimension) / std::max(source.width, source.height));
  const int intermediate_width = std::max(1, static_cast<int>(source.width * scale));
  const int intermediate_height = std::max(1, static_cast<int>(source.height * scale));
  return {
      std::max(
          FeatureMatcher::kLoFTRDimensionAlignment,
          intermediate_width / FeatureMatcher::kLoFTRDimensionAlignment * FeatureMatcher::kLoFTRDimensionAlignment),
      std::max(
          FeatureMatcher::kLoFTRDimensionAlignment,
          intermediate_height / FeatureMatcher::kLoFTRDimensionAlignment * FeatureMatcher::kLoFTRDimensionAlignment),
  };
}

absl::Status validate_preprocessing_metadata(const FeaturePairInput& input) {
  if (input.tensor_size.width <= 0 || input.tensor_size.height <= 0) {
    return absl::InvalidArgumentError("Feature preprocessing tensor size is invalid");
  }
  for (int image_index = 0; image_index < 2; ++image_index) {
    if (input.source_sizes[image_index].width <= 0 || input.source_sizes[image_index].height <= 0 ||
        input.resized_sizes[image_index].width <= 0 || input.resized_sizes[image_index].height <= 0 ||
        input.resized_sizes[image_index].width > input.tensor_size.width ||
        input.resized_sizes[image_index].height > input.tensor_size.height) {
      return absl::InvalidArgumentError("Feature preprocessing metadata is invalid");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<FeatureMatchResult> finish_matches(
    std::vector<FeatureMatch> accepted,
    cv::Size left_source_size,
    size_t max_control_points) {
  if (accepted.empty())
    return absl::NotFoundError("Feature matcher produced no usable matches");
  auto selected = FeatureMatcher::SelectControlPoints(accepted, left_source_size, max_control_points);
  if (!selected.ok())
    return selected.status();
  FeatureMatchResult result;
  result.accepted_match_count = accepted.size();
  result.accepted = std::move(accepted);
  result.selected = std::move(*selected);
  return result;
}

struct OneWayBinaryMatch {
  int query{-1};
  int train{-1};
  int distance{0};
};

std::vector<OneWayBinaryMatch> ratio_matches(const cv::Mat& query, const cv::Mat& train, float ratio) {
  if (query.empty() || train.empty())
    return {};
  cv::BFMatcher matcher(cv::NORM_HAMMING, false);
  std::vector<std::vector<cv::DMatch>> neighbors;
  matcher.knnMatch(query, train, neighbors, 2);
  std::vector<OneWayBinaryMatch> accepted;
  accepted.reserve(neighbors.size());
  for (size_t query_index = 0; query_index < neighbors.size(); ++query_index) {
    if (neighbors[query_index].empty())
      continue;
    const cv::DMatch& best = neighbors[query_index][0];
    const float second_distance = neighbors[query_index].size() > 1
        ? neighbors[query_index][1].distance
        : static_cast<float>(std::numeric_limits<int>::max());
    if (second_distance > 0.0f && best.distance < ratio * second_distance) {
      accepted.push_back({static_cast<int>(query_index), best.trainIdx, static_cast<int>(std::lround(best.distance))});
    }
  }
  return accepted;
}

void retain_strongest_keypoints(std::vector<cv::KeyPoint>* keypoints, size_t maximum) {
  std::stable_sort(keypoints->begin(), keypoints->end(), [](const cv::KeyPoint& left, const cv::KeyPoint& right) {
    return std::make_tuple(-left.response, left.pt.y, left.pt.x, left.size, left.angle, left.octave, left.class_id) <
        std::make_tuple(-right.response, right.pt.y, right.pt.x, right.size, right.angle, right.octave, right.class_id);
  });
  if (keypoints->size() > maximum)
    keypoints->resize(maximum);
}

struct AkazeFeatures {
  std::vector<cv::KeyPoint> keypoints;
  cv::Mat descriptors;
  cv::Size source_size;
  cv::Size detector_size;
  std::optional<cv::Matx33d> camera_matrix;
  std::optional<cv::Vec4d> distortion;
};

absl::Status validate_lens_calibration(const FisheyeLensCalibration& calibration) {
  if (calibration.resolution.width <= 0 || calibration.resolution.height <= 0 || !std::isfinite(calibration.fx) ||
      !std::isfinite(calibration.fy) || !std::isfinite(calibration.cx) || !std::isfinite(calibration.cy) ||
      calibration.fx <= 0.0 || calibration.fy <= 0.0) {
    return absl::InvalidArgumentError("AKAZE fisheye calibration has invalid camera intrinsics");
  }
  for (double coefficient : calibration.distortion) {
    if (!std::isfinite(coefficient))
      return absl::InvalidArgumentError("AKAZE fisheye calibration has a non-finite distortion coefficient");
  }
  return absl::OkStatus();
}

absl::StatusOr<AkazeFeatures> detect_akaze(
    const cv::Mat& source,
    bool left_camera,
    const std::optional<FisheyeLensCalibration>& calibration) {
  AkazeFeatures result;
  result.source_size = source.size();
  cv::Mat gray = grayscale_u8(source);
  const double scale =
      std::min(1.0, static_cast<double>(FeatureMatcher::kAkazeMaximumDimension) / std::max(gray.cols, gray.rows));
  result.detector_size = {
      std::max(1, static_cast<int>(std::round(gray.cols * scale))),
      std::max(1, static_cast<int>(std::round(gray.rows * scale))),
  };
  if (result.detector_size != gray.size()) {
    cv::resize(gray, gray, result.detector_size, 0.0, 0.0, cv::INTER_AREA);
  }
  if (calibration.has_value()) {
    auto calibration_status = validate_lens_calibration(*calibration);
    if (!calibration_status.ok())
      return calibration_status;
    const double scale_x = static_cast<double>(result.detector_size.width) / calibration->resolution.width;
    const double scale_y = static_cast<double>(result.detector_size.height) / calibration->resolution.height;
    result.camera_matrix = cv::Matx33d(
        calibration->fx * scale_x,
        0.0,
        calibration->cx * scale_x,
        0.0,
        calibration->fy * scale_y,
        calibration->cy * scale_y,
        0.0,
        0.0,
        1.0);
    result.distortion = cv::Vec4d(
        calibration->distortion[0], calibration->distortion[1], calibration->distortion[2], calibration->distortion[3]);
    cv::Mat map_x;
    cv::Mat map_y;
    cv::fisheye::initUndistortRectifyMap(
        *result.camera_matrix,
        *result.distortion,
        cv::Matx33d::eye(),
        *result.camera_matrix,
        result.detector_size,
        CV_32FC1,
        map_x,
        map_y);
    cv::Mat undistorted;
    cv::remap(gray, undistorted, map_x, map_y, cv::INTER_LINEAR, cv::BORDER_CONSTANT);
    gray = std::move(undistorted);
  }
  cv::Mat detection_mask(result.detector_size, CV_8UC1, cv::Scalar::all(0));
  constexpr double kDetectionYMinimum = 0.05;
  constexpr double kDetectionYMaximum = 0.95;
  const int x_begin = left_camera ? result.detector_size.width / 2 : 0;
  const int x_end = left_camera ? result.detector_size.width : (result.detector_size.width + 1) / 2;
  const int y_begin = static_cast<int>(std::floor(kDetectionYMinimum * result.detector_size.height));
  const int y_end = static_cast<int>(std::ceil(kDetectionYMaximum * result.detector_size.height));
  detection_mask(cv::Rect(x_begin, y_begin, x_end - x_begin, y_end - y_begin)).setTo(cv::Scalar::all(255));
  auto detector =
      cv::AKAZE::create(cv::AKAZE::DESCRIPTOR_MLDB, 0, 3, FeatureMatcher::kAkazeThreshold, 4, 4, cv::KAZE::DIFF_PM_G2);
  detector->detect(gray, result.keypoints, detection_mask);
  retain_strongest_keypoints(&result.keypoints, FeatureMatcher::kAkazeMaximumKeypoints);
  if (!result.keypoints.empty())
    detector->compute(gray, result.keypoints, result.descriptors);
  return result;
}

cv::Point2f akaze_rectified_source_point(const AkazeFeatures& features, const cv::Point2f& detector_point) {
  return {
      detector_point.x * features.source_size.width / features.detector_size.width,
      detector_point.y * features.source_size.height / features.detector_size.height,
  };
}

} // namespace

FeatureMatcher::FeatureMatcher(
    ControlPointMatcher matcher,
    std::unique_ptr<hm::onnx::Session> session,
    int input_channels,
    size_t sparse_keypoints_per_image,
    bool sparse_keypoints_are_float,
    AkazeMatchingCalibration akaze_calibration)
    : matcher_(matcher),
      session_(std::move(session)),
      input_channels_(input_channels),
      sparse_keypoints_per_image_(sparse_keypoints_per_image),
      sparse_keypoints_are_float_(sparse_keypoints_are_float),
      akaze_calibration_(std::move(akaze_calibration)) {}

absl::StatusOr<std::unique_ptr<FeatureMatcher>> FeatureMatcher::Create(
    const std::string& model_path,
    ControlPointMatcher matcher,
    AkazeMatchingCalibration akaze_calibration) {
  if (matcher == ControlPointMatcher::kAkazeHamming) {
    if (akaze_calibration.left.has_value() != akaze_calibration.right.has_value()) {
      return absl::InvalidArgumentError("AKAZE lens calibration must contain both cameras or neither camera");
    }
    if (akaze_calibration.left.has_value()) {
      auto status = validate_lens_calibration(*akaze_calibration.left);
      if (!status.ok())
        return status;
      status = validate_lens_calibration(*akaze_calibration.right);
      if (!status.ok())
        return status;
    }
    return std::unique_ptr<FeatureMatcher>(
        new FeatureMatcher(matcher, {}, 0, kKeypointsPerImage, false, std::move(akaze_calibration)));
  }
  if (model_path.empty()) {
    return absl::InvalidArgumentError(
        std::string("Native control-point matcher ") + ControlPointMatcherName(matcher) + " requires an ONNX model");
  }

  absl::StatusOr<std::unique_ptr<hm::onnx::Session>> session = absl::InternalError("Unknown feature matcher");
  int input_channels = 0;
  switch (matcher) {
    case ControlPointMatcher::kSuperPointLightGlue:
      input_channels = 1;
      session = hm::onnx::Session::Create(
          model_path,
          {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, input_channels, -1, -1}}},
          {
              {"keypoints", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {-1, kKeypointsPerImage, 2}},
              {"matches", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {-1, 3}},
              {"mscores", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1}},
          });
      break;
    case ControlPointMatcher::kDeDoDeLightGlue:
      input_channels = 3;
      session = hm::onnx::Session::Create(
          model_path,
          {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {2, input_channels, kInputHeight, kInputWidth}}},
          {
              {"keypoints", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {2, kKeypointsPerImage, 2}},
              {"matches0", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {1, kKeypointsPerImage}},
              {"matching_scores0", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kKeypointsPerImage}},
          });
      break;
    case ControlPointMatcher::kLoFTR:
      input_channels = 1;
      session = hm::onnx::Session::Create(
          model_path,
          {
              {"image0", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 1, -1, -1}},
              {"image1", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 1, -1, -1}},
          },
          {
              {"mkpts0_f", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, 2}},
              {"mkpts1_f", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, 2}},
              {"mconf", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1}},
          });
      break;
    case ControlPointMatcher::kAkazeHamming:
      break;
  }
  if (!session.ok())
    return session.status();
  return std::unique_ptr<FeatureMatcher>(new FeatureMatcher(matcher, std::move(*session), input_channels));
}

absl::StatusOr<std::unique_ptr<FeatureMatcher>> FeatureMatcher::CreateLegacyAlikedParity(
    const std::string& model_path) {
  auto session = hm::onnx::Session::Create(
      model_path,
      {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, 3, -1, -1}}},
      {
          {"keypoints", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, kLegacyAlikedKeypointsPerImage, 2}},
          {"matches", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {-1, 3}},
          {"mscores", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1}},
      });
  if (!session.ok())
    return session.status();
  return std::unique_ptr<FeatureMatcher>(new FeatureMatcher(
      ControlPointMatcher::kSuperPointLightGlue, std::move(*session), 3, kLegacyAlikedKeypointsPerImage, true));
}

absl::StatusOr<FeaturePairInput> FeatureMatcher::Prepare(const cv::Mat& left_bgr, const cv::Mat& right_bgr) {
  return prepare_feature_pair(left_bgr, right_bgr, 3);
}

absl::StatusOr<FeaturePairInput> FeatureMatcher::PrepareLoFTR(const cv::Mat& left_bgr, const cv::Mat& right_bgr) {
  auto status = validate_source_image(left_bgr, "Left");
  if (!status.ok())
    return status;
  status = validate_source_image(right_bgr, "Right");
  if (!status.ok())
    return status;

  FeaturePairInput result;
  result.source_sizes[0] = left_bgr.size();
  result.source_sizes[1] = right_bgr.size();
  result.resized_sizes[0] = aligned_loftr_size(left_bgr.size());
  result.resized_sizes[1] = aligned_loftr_size(right_bgr.size());
  result.tensor_size = {
      std::max(result.resized_sizes[0].width, result.resized_sizes[1].width),
      std::max(result.resized_sizes[0].height, result.resized_sizes[1].height),
  };
  const size_t image_plane = static_cast<size_t>(result.tensor_size.width) * result.tensor_size.height;
  result.tensor.assign(2 * image_plane, 0.0f);
  const cv::Mat* images[] = {&left_bgr, &right_bgr};
  for (int image_index = 0; image_index < 2; ++image_index) {
    cv::Mat gray = grayscale_u8(*images[image_index]);
    cv::resize(gray, gray, result.resized_sizes[image_index], 0.0, 0.0, cv::INTER_AREA);
    const size_t image_base = static_cast<size_t>(image_index) * image_plane;
    for (int y = 0; y < gray.rows; ++y) {
      const uchar* row = gray.ptr<uchar>(y);
      float* output = result.tensor.data() + image_base + static_cast<size_t>(y) * result.tensor_size.width;
      for (int x = 0; x < gray.cols; ++x)
        output[x] = static_cast<float>(row[x]) / 255.0f;
    }
  }
  return result;
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::Postprocess(
    const FeaturePairInput& input,
    const float* keypoints,
    size_t keypoint_count,
    const int64_t* matches,
    size_t match_value_count,
    const float* scores,
    size_t score_count,
    size_t max_control_points) {
  return PostprocessSparse(
      input,
      keypoints,
      keypoint_count,
      matches,
      match_value_count,
      scores,
      score_count,
      max_control_points,
      kKeypointsPerImage);
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::PostprocessSparse(
    const FeaturePairInput& input,
    const float* keypoints,
    size_t keypoint_count,
    const int64_t* matches,
    size_t match_value_count,
    const float* scores,
    size_t score_count,
    size_t max_control_points,
    size_t keypoints_per_image) {
  if (keypoints_per_image == 0 || keypoints_per_image > static_cast<size_t>(std::numeric_limits<int>::max()))
    return absl::InvalidArgumentError("Feature matcher keypoint capacity is invalid");
  const size_t expected_keypoints = static_cast<size_t>(2) * keypoints_per_image * 2;
  if (keypoints == nullptr || keypoint_count != expected_keypoints) {
    return absl::InvalidArgumentError("Feature matcher keypoints violate the frozen output contract");
  }
  if (match_value_count % 3 != 0 || match_value_count / 3 != score_count ||
      (score_count > 0 && (matches == nullptr || scores == nullptr))) {
    return absl::InvalidArgumentError("LightGlue matches and scores have inconsistent shapes");
  }
  if (max_control_points == 0) {
    return absl::InvalidArgumentError("Maximum control point count must be positive");
  }
  for (int image_index = 0; image_index < 2; ++image_index) {
    if (input.source_sizes[image_index].width <= 0 || input.source_sizes[image_index].height <= 0 ||
        input.resized_sizes[image_index].width <= 0 || input.resized_sizes[image_index].height <= 0 ||
        input.resized_sizes[image_index].width > kInputWidth ||
        input.resized_sizes[image_index].height > kInputHeight) {
      return absl::InvalidArgumentError("Feature preprocessing metadata is invalid");
    }
  }

  std::vector<FeatureMatch> accepted;
  accepted.reserve(score_count);
  for (size_t match_index = 0; match_index < score_count; ++match_index) {
    const int64_t pair = matches[match_index * 3];
    const int64_t left_index = matches[match_index * 3 + 1];
    const int64_t right_index = matches[match_index * 3 + 2];
    if (pair != 0 || left_index < 0 || left_index >= static_cast<int64_t>(keypoints_per_image) || right_index < 0 ||
        right_index >= static_cast<int64_t>(keypoints_per_image)) {
      return absl::OutOfRangeError("LightGlue returned an invalid pair or keypoint index");
    }
    const float score = scores[match_index];
    if (!std::isfinite(score))
      return absl::InvalidArgumentError("LightGlue returned a non-finite score");
    if (!(score > kMinimumScore))
      continue;
    const float* left = keypoints + static_cast<size_t>(left_index) * 2;
    const float* right = keypoints + (keypoints_per_image + right_index) * 2;
    if (!std::isfinite(left[0]) || !std::isfinite(left[1]) || !std::isfinite(right[0]) || !std::isfinite(right[1])) {
      return absl::InvalidArgumentError("Feature matcher returned a non-finite keypoint");
    }
    if (left[0] < 0.0f || left[1] < 0.0f || right[0] < 0.0f || right[1] < 0.0f ||
        left[0] >= input.resized_sizes[0].width || left[1] >= input.resized_sizes[0].height ||
        right[0] >= input.resized_sizes[1].width || right[1] >= input.resized_sizes[1].height) {
      // Matches in a zero-padded region are not valid source coordinates.
      continue;
    }
    accepted.push_back({
        {(left[0] + 0.5f) * input.source_sizes[0].width / input.resized_sizes[0].width - 0.5f,
         (left[1] + 0.5f) * input.source_sizes[0].height / input.resized_sizes[0].height - 0.5f},
        {(right[0] + 0.5f) * input.source_sizes[1].width / input.resized_sizes[1].width - 0.5f,
         (right[1] + 0.5f) * input.source_sizes[1].height / input.resized_sizes[1].height - 0.5f},
        score,
        static_cast<int>(left_index),
        static_cast<int>(right_index),
    });
  }
  if (accepted.empty())
    return absl::NotFoundError("Feature matcher produced no usable matches");

  auto selected = SelectControlPoints(accepted, input.source_sizes[0], max_control_points);
  if (!selected.ok())
    return selected.status();
  FeatureMatchResult result;
  result.accepted_match_count = accepted.size();
  result.accepted = std::move(accepted);
  result.selected = std::move(*selected);
  return result;
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::PostprocessDeDoDe(
    const FeaturePairInput& input,
    const float* keypoints,
    size_t keypoint_count,
    const int64_t* matches,
    size_t match_count,
    const float* scores,
    size_t score_count,
    size_t max_control_points) {
  constexpr size_t expected_keypoints = static_cast<size_t>(2) * kKeypointsPerImage * 2;
  if (keypoints == nullptr || keypoint_count != expected_keypoints || matches == nullptr ||
      match_count != kKeypointsPerImage || scores == nullptr || score_count != kKeypointsPerImage) {
    return absl::InvalidArgumentError("DeDoDe + LightGlue outputs violate the frozen contract");
  }
  if (max_control_points == 0)
    return absl::InvalidArgumentError("Maximum control point count must be positive");
  auto metadata_status = validate_preprocessing_metadata(input);
  if (!metadata_status.ok())
    return metadata_status;

  std::vector<FeatureMatch> accepted;
  accepted.reserve(kKeypointsPerImage);
  for (int left_index = 0; left_index < kKeypointsPerImage; ++left_index) {
    const int64_t right_index = matches[left_index];
    const float score = scores[left_index];
    if (right_index < -1 || right_index >= kKeypointsPerImage)
      return absl::OutOfRangeError("DeDoDe + LightGlue returned an invalid keypoint index");
    if (!std::isfinite(score))
      return absl::InvalidArgumentError("DeDoDe + LightGlue returned a non-finite score");
    if (right_index < 0 || !(score > kMinimumScore))
      continue;
    const float* left = keypoints + static_cast<size_t>(left_index) * 2;
    const float* right = keypoints + (static_cast<size_t>(kKeypointsPerImage) + right_index) * 2;
    if (!std::isfinite(left[0]) || !std::isfinite(left[1]) || !std::isfinite(right[0]) || !std::isfinite(right[1]))
      return absl::InvalidArgumentError("DeDoDe + LightGlue returned a non-finite keypoint");
    if (left[0] < 0.0f || left[1] < 0.0f || right[0] < 0.0f || right[1] < 0.0f ||
        left[0] >= input.resized_sizes[0].width || left[1] >= input.resized_sizes[0].height ||
        right[0] >= input.resized_sizes[1].width || right[1] >= input.resized_sizes[1].height) {
      continue;
    }
    accepted.push_back({
        {left[0] * input.source_sizes[0].width / input.resized_sizes[0].width,
         left[1] * input.source_sizes[0].height / input.resized_sizes[0].height},
        {right[0] * input.source_sizes[1].width / input.resized_sizes[1].width,
         right[1] * input.source_sizes[1].height / input.resized_sizes[1].height},
        score,
        left_index,
        static_cast<int>(right_index),
    });
  }
  return finish_matches(std::move(accepted), input.source_sizes[0], max_control_points);
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::PostprocessLoFTR(
    const FeaturePairInput& input,
    const float* left_keypoints,
    size_t left_keypoint_count,
    const float* right_keypoints,
    size_t right_keypoint_count,
    const float* scores,
    size_t score_count,
    size_t max_control_points) {
  if (left_keypoint_count != score_count * 2 || right_keypoint_count != score_count * 2 ||
      (score_count > 0 && (left_keypoints == nullptr || right_keypoints == nullptr || scores == nullptr))) {
    return absl::InvalidArgumentError("LoFTR keypoints and scores have inconsistent shapes");
  }
  if (max_control_points == 0)
    return absl::InvalidArgumentError("Maximum control point count must be positive");
  auto metadata_status = validate_preprocessing_metadata(input);
  if (!metadata_status.ok())
    return metadata_status;

  std::vector<FeatureMatch> accepted;
  accepted.reserve(score_count);
  for (size_t index = 0; index < score_count; ++index) {
    const float* left = left_keypoints + index * 2;
    const float* right = right_keypoints + index * 2;
    const float score = scores[index];
    if (!std::isfinite(left[0]) || !std::isfinite(left[1]) || !std::isfinite(right[0]) || !std::isfinite(right[1]) ||
        !std::isfinite(score)) {
      return absl::InvalidArgumentError("LoFTR returned a non-finite match");
    }
    if (!(score > kMinimumScore) || left[0] < 0.0f || left[1] < 0.0f || right[0] < 0.0f || right[1] < 0.0f ||
        left[0] >= input.resized_sizes[0].width || left[1] >= input.resized_sizes[0].height ||
        right[0] >= input.resized_sizes[1].width || right[1] >= input.resized_sizes[1].height) {
      continue;
    }
    accepted.push_back({
        {left[0] * input.source_sizes[0].width / input.resized_sizes[0].width,
         left[1] * input.source_sizes[0].height / input.resized_sizes[0].height},
        {right[0] * input.source_sizes[1].width / input.resized_sizes[1].width,
         right[1] * input.source_sizes[1].height / input.resized_sizes[1].height},
        score,
        static_cast<int>(index),
        static_cast<int>(index),
    });
  }
  return finish_matches(std::move(accepted), input.source_sizes[0], max_control_points);
}

absl::StatusOr<std::vector<FeatureMatch>> FeatureMatcher::SelectControlPoints(
    const std::vector<FeatureMatch>& accepted,
    cv::Size left_source_size,
    size_t max_control_points) {
  if (max_control_points == 0) {
    return absl::InvalidArgumentError("Maximum control point count must be positive");
  }
  if (left_source_size.width <= 0 || left_source_size.height <= 0) {
    return absl::InvalidArgumentError("Feature match selection source size is invalid");
  }
  if (accepted.empty()) {
    return absl::NotFoundError("Feature matcher produced no usable matches");
  }

  // Select a spatially distributed, deterministic subset. The former global
  // Y-rank linspace duplicated points when the requested cap exceeded the
  // model output and allowed one marginal match to shift every later rank.
  // Fixed cells localize those changes and make model-row permutations
  // irrelevant. A total final order keeps the emitted PTO reproducible.
  constexpr size_t grid_columns = 16;
  constexpr size_t grid_rows = 9;
  std::vector<std::vector<size_t>> cells(grid_columns * grid_rows);
  for (size_t index = 0; index < accepted.size(); ++index) {
    const double normalized_x =
        std::clamp(static_cast<double>(accepted[index].left.x) / left_source_size.width, 0.0, 1.0);
    const double normalized_y =
        std::clamp(static_cast<double>(accepted[index].left.y) / left_source_size.height, 0.0, 1.0);
    const size_t column = std::min(grid_columns - 1, static_cast<size_t>(normalized_x * grid_columns));
    const size_t row = std::min(grid_rows - 1, static_cast<size_t>(normalized_y * grid_rows));
    cells[row * grid_columns + column].push_back(index);
  }
  const auto ranked = [&](size_t lhs, size_t rhs) {
    const FeatureMatch& left = accepted[lhs];
    const FeatureMatch& right = accepted[rhs];
    const auto key = [](const FeatureMatch& match) {
      return std::make_tuple(
          -match.score, match.left.y, match.left.x, match.right.y, match.right.x, match.left_index, match.right_index);
    };
    return key(left) < key(right);
  };
  for (auto& cell : cells)
    std::sort(cell.begin(), cell.end(), ranked);

  const size_t selection_count = std::min(max_control_points, accepted.size());
  std::vector<size_t> selected_indices;
  selected_indices.reserve(selection_count);
  for (size_t rank = 0; selected_indices.size() < selection_count; ++rank) {
    bool added = false;
    for (const auto& cell : cells) {
      if (rank < cell.size()) {
        selected_indices.push_back(cell[rank]);
        added = true;
        if (selected_indices.size() == selection_count)
          break;
      }
    }
    if (!added)
      break;
  }
  std::sort(selected_indices.begin(), selected_indices.end(), [&](size_t lhs, size_t rhs) {
    const FeatureMatch& left = accepted[lhs];
    const FeatureMatch& right = accepted[rhs];
    const auto key = [](const FeatureMatch& match) {
      return std::make_tuple(
          match.left.y, match.left.x, match.right.y, match.right.x, match.left_index, match.right_index, -match.score);
    };
    return key(left) < key(right);
  });
  std::vector<FeatureMatch> selected;
  selected.reserve(selection_count);
  for (size_t index : selected_indices)
    selected.push_back(accepted[index]);
  return selected;
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::Infer(
    const cv::Mat& left_bgr,
    const cv::Mat& right_bgr,
    size_t max_control_points,
    const std::function<void()>& inference_complete,
    const std::function<bool()>& is_cancelled) const {
  if (is_cancelled && is_cancelled()) {
    return absl::CancelledError("Feature matching cancelled before preprocessing");
  }
  if (matcher_ == ControlPointMatcher::kAkazeHamming) {
    return InferAkaze(left_bgr, right_bgr, max_control_points, inference_complete, is_cancelled);
  }
  if (!session_)
    return absl::FailedPreconditionError("Feature matcher has no inference session");

  auto input = matcher_ == ControlPointMatcher::kLoFTR ? PrepareLoFTR(left_bgr, right_bgr)
                                                       : prepare_feature_pair(left_bgr, right_bgr, input_channels_);
  if (!input.ok())
    return input.status();
  absl::StatusOr<std::vector<hm::onnx::Tensor>> outputs = absl::InternalError("Unknown feature matcher");
  if (matcher_ == ControlPointMatcher::kLoFTR) {
    const size_t image_plane = static_cast<size_t>(input->tensor_size.width) * input->tensor_size.height;
    const std::vector<int64_t> shape = {1, 1, input->tensor_size.height, input->tensor_size.width};
    outputs = session_->RunFloatInputs(
        {
            {"image0", shape, input->tensor.data(), image_plane},
            {"image1", shape, input->tensor.data() + image_plane, image_plane},
        },
        is_cancelled);
  } else {
    outputs = session_->RunFloat(
        "images",
        {2, input_channels_, kInputHeight, kInputWidth},
        input->tensor.data(),
        input->tensor.size(),
        is_cancelled);
  }
  if (!outputs.ok())
    return outputs.status();
  if (outputs->size() != 3)
    return absl::InternalError("Feature model returned an unexpected output count");
  if (inference_complete)
    inference_complete();

  auto first_count = outputs->at(0).element_count();
  auto second_count = outputs->at(1).element_count();
  auto score_count = outputs->at(2).element_count();
  if (!first_count.ok())
    return first_count.status();
  if (!second_count.ok())
    return second_count.status();
  if (!score_count.ok())
    return score_count.status();
  auto scores = outputs->at(2).float_data();
  if (!scores.ok())
    return scores.status();

  if (matcher_ == ControlPointMatcher::kLoFTR) {
    auto left_keypoints = outputs->at(0).float_data();
    auto right_keypoints = outputs->at(1).float_data();
    if (!left_keypoints.ok())
      return left_keypoints.status();
    if (!right_keypoints.ok())
      return right_keypoints.status();
    return PostprocessLoFTR(
        *input,
        *left_keypoints,
        *first_count,
        *right_keypoints,
        *second_count,
        *scores,
        *score_count,
        max_control_points);
  }

  auto matches = outputs->at(1).int64_data();
  if (!matches.ok())
    return matches.status();
  if (matcher_ == ControlPointMatcher::kDeDoDeLightGlue) {
    auto keypoints = outputs->at(0).float_data();
    if (!keypoints.ok())
      return keypoints.status();
    return PostprocessDeDoDe(
        *input, *keypoints, *first_count, *matches, *second_count, *scores, *score_count, max_control_points);
  }

  if (sparse_keypoints_are_float_) {
    auto keypoints = outputs->at(0).float_data();
    if (!keypoints.ok())
      return keypoints.status();
    return PostprocessSparse(
        *input,
        *keypoints,
        *first_count,
        *matches,
        *second_count,
        *scores,
        *score_count,
        max_control_points,
        sparse_keypoints_per_image_);
  }
  auto keypoints = outputs->at(0).int64_data();
  if (!keypoints.ok())
    return keypoints.status();
  std::vector<float> keypoint_values(*first_count);
  for (size_t index = 0; index < *first_count; ++index)
    keypoint_values[index] = static_cast<float>((*keypoints)[index]);
  return PostprocessSparse(
      *input,
      keypoint_values.data(),
      keypoint_values.size(),
      *matches,
      *second_count,
      *scores,
      *score_count,
      max_control_points,
      sparse_keypoints_per_image_);
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::InferAkaze(
    const cv::Mat& left_bgr,
    const cv::Mat& right_bgr,
    size_t max_control_points,
    const std::function<void()>& inference_complete,
    const std::function<bool()>& is_cancelled) const {
  if (max_control_points == 0)
    return absl::InvalidArgumentError("Maximum control point count must be positive");
  auto status = validate_source_image(left_bgr, "Left");
  if (!status.ok())
    return status;
  status = validate_source_image(right_bgr, "Right");
  if (!status.ok())
    return status;
  auto left = detect_akaze(left_bgr, true, akaze_calibration_.left);
  if (!left.ok())
    return left.status();
  if (is_cancelled && is_cancelled())
    return absl::CancelledError("AKAZE feature matching cancelled after left-image detection");
  auto right = detect_akaze(right_bgr, false, akaze_calibration_.right);
  if (!right.ok())
    return right.status();
  if (is_cancelled && is_cancelled())
    return absl::CancelledError("AKAZE feature matching cancelled after right-image detection");
  if (left->descriptors.empty() || right->descriptors.empty())
    return absl::NotFoundError("AKAZE produced no usable M-LDB descriptors");

  const auto forward = ratio_matches(left->descriptors, right->descriptors, kAkazeLoweRatio);
  const auto backward = ratio_matches(right->descriptors, left->descriptors, kAkazeLoweRatio);
  std::vector<int> right_to_left(right->keypoints.size(), -1);
  for (const auto& match : backward) {
    if (match.query >= 0 && static_cast<size_t>(match.query) < right_to_left.size())
      right_to_left[match.query] = match.train;
  }
  const float descriptor_bits = static_cast<float>(left->descriptors.cols * 8);
  struct CandidateMatch {
    cv::Point2f left;
    cv::Point2f right;
    float score;
    int left_index;
    int right_index;
  };
  std::vector<CandidateMatch> candidates;
  candidates.reserve(forward.size());
  for (const auto& match : forward) {
    if (match.train < 0 || static_cast<size_t>(match.train) >= right_to_left.size() ||
        right_to_left[match.train] != match.query) {
      continue;
    }
    const cv::Point2f left_point = left->keypoints[match.query].pt;
    const cv::Point2f right_point = right->keypoints[match.train].pt;
    const float left_x = left_point.x / left->detector_size.width;
    const float right_x = right_point.x / right->detector_size.width;
    const float left_y = left_point.y / left->detector_size.height;
    const float right_y = right_point.y / right->detector_size.height;
    if (left_x < 0.5f || right_x > 0.5f || left_y < 0.2f || left_y > 0.8f || right_y < 0.2f || right_y > 0.8f ||
        std::abs(left_y - right_y) > 0.08f) {
      continue;
    }
    candidates.push_back({
        left_point,
        right_point,
        1.0f - static_cast<float>(match.distance) / descriptor_bits,
        match.query,
        match.train,
    });
  }
  if (candidates.size() < 8) {
    return absl::NotFoundError("AKAZE produced fewer than eight mutual overlap matches for epipolar filtering");
  }

  std::vector<cv::Point2f> left_points;
  std::vector<cv::Point2f> right_points;
  left_points.reserve(candidates.size());
  right_points.reserve(candidates.size());
  for (const CandidateMatch& candidate : candidates) {
    left_points.push_back(candidate.left);
    right_points.push_back(candidate.right);
  }
  cv::Mat inlier_mask;
  const cv::Mat fundamental =
      cv::findFundamentalMat(left_points, right_points, cv::FM_RANSAC, 1.0, 0.99, 2000, inlier_mask);
  if (fundamental.empty() || inlier_mask.total() != candidates.size()) {
    return absl::NotFoundError("AKAZE could not estimate a fundamental matrix for overlap matches");
  }
  std::vector<FeatureMatch> accepted;
  accepted.reserve(candidates.size());
  for (size_t index = 0; index < candidates.size(); ++index) {
    if (inlier_mask.ptr<unsigned char>()[index] == 0)
      continue;
    const CandidateMatch& candidate = candidates[index];
    accepted.push_back({
        akaze_rectified_source_point(*left, candidate.left),
        akaze_rectified_source_point(*right, candidate.right),
        candidate.score,
        candidate.left_index,
        candidate.right_index,
    });
  }
  if (accepted.size() < 6) {
    return absl::NotFoundError("AKAZE fundamental-matrix filtering retained fewer than six matches");
  }
  std::stable_sort(
      accepted.begin(), accepted.end(), [](const FeatureMatch& left_match, const FeatureMatch& right_match) {
        return std::make_tuple(-left_match.score, left_match.left_index, left_match.right_index) <
            std::make_tuple(-right_match.score, right_match.left_index, right_match.right_index);
      });
  if (inference_complete)
    inference_complete();
  return finish_matches(std::move(accepted), left_bgr.size(), max_control_points);
}

} // namespace hm::stitching
