#include "hstream/src/libs/stitching/FeatureMatcher.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

absl::Status validate_source_image(const cv::Mat& image, const char* side) {
  if (image.empty() || image.type() != CV_8UC3) {
    return absl::InvalidArgumentError(std::string(side) + " feature image must be non-empty CV_8UC3 BGR");
  }
  return absl::OkStatus();
}

} // namespace

FeatureMatcher::FeatureMatcher(std::unique_ptr<hm::onnx::Session> session) : session_(std::move(session)) {}

absl::StatusOr<std::unique_ptr<FeatureMatcher>> FeatureMatcher::Create(const std::string& model_path) {
  auto session = hm::onnx::Session::Create(
      model_path,
      {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, 3, -1, -1}}},
      {
          {"keypoints", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1, kKeypointsPerImage, 2}},
          {"matches", ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, {-1, 3}},
          {"mscores", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {-1}},
      });
  if (!session.ok())
    return session.status();
  return std::unique_ptr<FeatureMatcher>(new FeatureMatcher(std::move(*session)));
}

absl::StatusOr<FeaturePairInput> FeatureMatcher::Prepare(const cv::Mat& left_bgr, const cv::Mat& right_bgr) {
  auto status = validate_source_image(left_bgr, "Left");
  if (!status.ok())
    return status;
  status = validate_source_image(right_bgr, "Right");
  if (!status.ok())
    return status;

  FeaturePairInput result;
  result.source_sizes[0] = left_bgr.size();
  result.source_sizes[1] = right_bgr.size();
  const size_t image_plane = static_cast<size_t>(kInputHeight) * kInputWidth;
  result.tensor.assign(2 * 3 * image_plane, 0.0f);
  const cv::Mat* images[] = {&left_bgr, &right_bgr};
  for (int image_index = 0; image_index < 2; ++image_index) {
    const cv::Mat& source = *images[image_index];
    const double scale =
        std::min(static_cast<double>(kInputWidth) / source.cols, static_cast<double>(kInputHeight) / source.rows);
    const int width = std::max(32, static_cast<int>(std::round(source.cols * scale)));
    const int height = std::max(32, static_cast<int>(std::round(source.rows * scale)));
    if (width > kInputWidth || height > kInputHeight) {
      return absl::InternalError("Feature resize exceeded its fixed ONNX canvas");
    }
    result.resized_sizes[image_index] = {width, height};
    cv::Mat resized;
    cv::resize(source, resized, {width, height}, 0.0, 0.0, cv::INTER_AREA);
    const size_t image_base = static_cast<size_t>(image_index) * 3 * image_plane;
    for (int y = 0; y < height; ++y) {
      const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
      for (int x = 0; x < width; ++x) {
        for (int channel = 0; channel < 3; ++channel) {
          result.tensor
              [image_base + static_cast<size_t>(channel) * image_plane + static_cast<size_t>(y) * kInputWidth + x] =
              row[x][2 - channel] / 255.0f;
        }
      }
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
  constexpr size_t expected_keypoints = static_cast<size_t>(2) * kKeypointsPerImage * 2;
  if (keypoints == nullptr || keypoint_count != expected_keypoints) {
    return absl::InvalidArgumentError("ALIKED keypoints violate the frozen output contract");
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
    if (pair != 0 || left_index < 0 || left_index >= kKeypointsPerImage || right_index < 0 ||
        right_index >= kKeypointsPerImage) {
      return absl::OutOfRangeError("LightGlue returned an invalid pair or keypoint index");
    }
    const float score = scores[match_index];
    if (!std::isfinite(score))
      return absl::InvalidArgumentError("LightGlue returned a non-finite score");
    if (!(score > kMinimumScore))
      continue;
    const float* left = keypoints + static_cast<size_t>(left_index) * 2;
    const float* right = keypoints + (static_cast<size_t>(kKeypointsPerImage) + right_index) * 2;
    if (!std::isfinite(left[0]) || !std::isfinite(left[1]) || !std::isfinite(right[0]) || !std::isfinite(right[1])) {
      return absl::InvalidArgumentError("ALIKED returned a non-finite keypoint");
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
    return absl::NotFoundError("ALIKED+LightGlue produced no usable matches");

  // Select a spatially distributed, deterministic subset. The former global
  // Y-rank linspace duplicated points when the requested cap exceeded the
  // model output and allowed one marginal match to shift every later rank.
  // Fixed cells localize those changes and make model-row permutations
  // irrelevant. A total final order keeps the emitted PTO reproducible.
  constexpr size_t grid_columns = 16;
  constexpr size_t grid_rows = 9;
  std::vector<std::vector<size_t>> cells(grid_columns * grid_rows);
  for (size_t index = 0; index < accepted.size(); ++index) {
    const size_t column = std::min(
        grid_columns - 1, static_cast<size_t>(accepted[index].left.x / input.source_sizes[0].width * grid_columns));
    const size_t row =
        std::min(grid_rows - 1, static_cast<size_t>(accepted[index].left.y / input.source_sizes[0].height * grid_rows));
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
  FeatureMatchResult result;
  result.accepted_match_count = accepted.size();
  result.accepted = accepted;
  result.selected.reserve(selection_count);
  for (size_t index : selected_indices)
    result.selected.push_back(accepted[index]);
  return result;
}

absl::StatusOr<FeatureMatchResult> FeatureMatcher::Infer(
    const cv::Mat& left_bgr,
    const cv::Mat& right_bgr,
    size_t max_control_points) const {
  auto input = Prepare(left_bgr, right_bgr);
  if (!input.ok())
    return input.status();
  auto outputs =
      session_->RunFloat("images", {2, 3, kInputHeight, kInputWidth}, input->tensor.data(), input->tensor.size());
  if (!outputs.ok())
    return outputs.status();
  if (outputs->size() != 3)
    return absl::InternalError("Feature model returned an unexpected output count");
  auto keypoints = outputs->at(0).float_data();
  auto matches = outputs->at(1).int64_data();
  auto scores = outputs->at(2).float_data();
  auto keypoint_count = outputs->at(0).element_count();
  auto match_count = outputs->at(1).element_count();
  auto score_count = outputs->at(2).element_count();
  if (!keypoints.ok())
    return keypoints.status();
  if (!matches.ok())
    return matches.status();
  if (!scores.ok())
    return scores.status();
  if (!keypoint_count.ok())
    return keypoint_count.status();
  if (!match_count.ok())
    return match_count.status();
  if (!score_count.ok())
    return score_count.status();
  return Postprocess(
      *input, *keypoints, *keypoint_count, *matches, *match_count, *scores, *score_count, max_control_points);
}

} // namespace hm::stitching
