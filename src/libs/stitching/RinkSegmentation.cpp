#include "hstream/src/libs/stitching/RinkSegmentation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>

#include <opencv2/imgproc.hpp>

#include "absl/status/status.h"

namespace hm::stitching {
namespace {

constexpr float kMean[] = {123.675f, 116.28f, 103.53f};
constexpr float kStd[] = {58.395f, 57.12f, 57.375f};
constexpr int kRinkClass = 1;
constexpr int kClassCount = RinkSegmentation::kClassCountWithBackground - 1;
constexpr int kMaxPerImage = 10;
constexpr float kScoreThreshold = 0.3f;

float sigmoid(float value) {
  if (value >= 0.0f)
    return 1.0f / (1.0f + std::exp(-value));
  const float exponential = std::exp(value);
  return exponential / (1.0f + exponential);
}

struct Candidate {
  float class_score{0.0f};
  int query{0};
  int label{0};
};

std::vector<Candidate> top_candidates(const float* logits) {
  std::vector<Candidate> candidates;
  candidates.reserve(RinkSegmentation::kQueryCount * kClassCount);
  for (int query = 0; query < RinkSegmentation::kQueryCount; ++query) {
    const float* row = logits + query * RinkSegmentation::kClassCountWithBackground;
    const float maximum = *std::max_element(row, row + RinkSegmentation::kClassCountWithBackground);
    double denominator = 0.0;
    for (int label = 0; label < RinkSegmentation::kClassCountWithBackground; ++label) {
      denominator += std::exp(static_cast<double>(row[label] - maximum));
    }
    for (int label = 0; label < kClassCount; ++label) {
      candidates.push_back(
          {static_cast<float>(std::exp(static_cast<double>(row[label] - maximum)) / denominator), query, label});
    }
  }
  std::partial_sort(
      candidates.begin(),
      candidates.begin() + kMaxPerImage,
      candidates.end(),
      [](const Candidate& lhs, const Candidate& rhs) {
        if (lhs.class_score != rhs.class_score)
          return lhs.class_score > rhs.class_score;
        if (lhs.query != rhs.query)
          return lhs.query < rhs.query;
        return lhs.label < rhs.label;
      });
  candidates.resize(kMaxPerImage);
  return candidates;
}

cv::Point2d contour_centroid(const std::vector<cv::Mat>& masks) {
  cv::Point2d sum(0.0, 0.0);
  size_t count = 0;
  for (const cv::Mat& mask : masks) {
    std::vector<std::vector<cv::Point>> contours;
    std::vector<cv::Vec4i> hierarchy;
    cv::findContours(mask.clone(), contours, hierarchy, cv::RETR_CCOMP, cv::CHAIN_APPROX_NONE);
    for (const auto& contour : contours) {
      for (const auto& point : contour) {
        sum.x += point.x;
        sum.y += point.y;
        ++count;
      }
    }
  }
  if (count == 0)
    return {};
  return {sum.x / static_cast<double>(count), sum.y / static_cast<double>(count)};
}

absl::Status validate_profile_geometry(const RinkProfile& profile, int width, int height) {
  if (!std::isfinite(profile.centroid.x) || !std::isfinite(profile.centroid.y) ||
      !std::isfinite(profile.combined_bbox.x) || !std::isfinite(profile.combined_bbox.y) ||
      !std::isfinite(profile.combined_bbox.width) || !std::isfinite(profile.combined_bbox.height) ||
      profile.combined_bbox.x < 0.0 || profile.combined_bbox.y < 0.0 || profile.combined_bbox.width <= 0.0 ||
      profile.combined_bbox.height <= 0.0 || profile.combined_bbox.x + profile.combined_bbox.width > width + 1.0 ||
      profile.combined_bbox.y + profile.combined_bbox.height > height + 1.0) {
    return absl::DataLossError("Ice-rink profile contains invalid geometry");
  }
  if (!std::all_of(profile.scores.begin(), profile.scores.end(), [](float score) {
        return std::isfinite(score) && score >= 0.0f && score <= 1.0f;
      })) {
    return absl::DataLossError("Ice-rink profile contains an invalid score");
  }
  return absl::OkStatus();
}

} // namespace

RinkSegmentation::RinkSegmentation(std::unique_ptr<hm::onnx::Session> session) : session_(std::move(session)) {}

absl::StatusOr<std::unique_ptr<RinkSegmentation>> RinkSegmentation::Create(const std::string& model_path) {
  auto session = hm::onnx::Session::Create(
      model_path,
      {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 3, kInputHeight, kInputWidth}}},
      {
          {"class_logits", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kQueryCount, kClassCountWithBackground}},
          {"mask_logits", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kQueryCount, kMaskHeight, kMaskWidth}},
      });
  if (!session.ok())
    return session.status();
  return std::unique_ptr<RinkSegmentation>(new RinkSegmentation(std::move(*session)));
}

absl::StatusOr<RinkInput> RinkSegmentation::Prepare(const cv::Mat& bgr_image) {
  if (bgr_image.empty() || bgr_image.type() != CV_8UC3) {
    return absl::InvalidArgumentError("Rink input must be a non-empty CV_8UC3 BGR image");
  }
  const double scale = std::min(1333.0 / bgr_image.cols, 800.0 / bgr_image.rows);
  const int resized_width = std::max(1, static_cast<int>(std::round(bgr_image.cols * scale)));
  const int resized_height = std::max(1, static_cast<int>(std::round(bgr_image.rows * scale)));
  if (resized_width > kInputWidth || resized_height > kInputHeight) {
    return absl::InternalError("Rink resize exceeded the fixed ONNX input canvas");
  }
  cv::Mat resized;
  cv::resize(bgr_image, resized, {resized_width, resized_height}, 0.0, 0.0, cv::INTER_LINEAR);

  RinkInput result;
  result.source_width = bgr_image.cols;
  result.source_height = bgr_image.rows;
  result.resized_width = resized_width;
  result.resized_height = resized_height;
  result.tensor.assign(static_cast<size_t>(3) * kInputHeight * kInputWidth, 0.0f);
  const size_t plane = static_cast<size_t>(kInputHeight) * kInputWidth;
  for (int y = 0; y < resized_height; ++y) {
    const cv::Vec3b* row = resized.ptr<cv::Vec3b>(y);
    for (int x = 0; x < resized_width; ++x) {
      // MMDetection's data preprocessor converts BGR to RGB before applying
      // ImageNet channel statistics. Padding is normalized-space zero.
      for (int channel = 0; channel < 3; ++channel) {
        const float rgb = row[x][2 - channel];
        result.tensor[static_cast<size_t>(channel) * plane + static_cast<size_t>(y) * kInputWidth + x] =
            (rgb - kMean[channel]) / kStd[channel];
      }
    }
  }
  return result;
}

absl::StatusOr<RinkProfile> RinkSegmentation::Postprocess(
    const RinkInput& input,
    const float* class_logits,
    size_t class_logit_count,
    const float* mask_logits,
    size_t mask_logit_count) {
  constexpr size_t expected_classes = static_cast<size_t>(kQueryCount) * kClassCountWithBackground;
  constexpr size_t expected_masks = static_cast<size_t>(kQueryCount) * kMaskHeight * kMaskWidth;
  if (class_logits == nullptr || mask_logits == nullptr || class_logit_count != expected_classes ||
      mask_logit_count != expected_masks) {
    return absl::InvalidArgumentError("Rink ONNX output sizes do not match the frozen model contract");
  }
  if (input.source_width <= 0 || input.source_height <= 0 || input.resized_width <= 0 || input.resized_height <= 0 ||
      input.resized_width > kInputWidth || input.resized_height > kInputHeight) {
    return absl::InvalidArgumentError("Rink preprocessing metadata is invalid");
  }
  if (!std::all_of(class_logits, class_logits + class_logit_count, [](float value) { return std::isfinite(value); }) ||
      !std::all_of(mask_logits, mask_logits + mask_logit_count, [](float value) { return std::isfinite(value); })) {
    return absl::DataLossError("Rink ONNX output contains non-finite values");
  }

  RinkProfile result;
  for (const Candidate& candidate : top_candidates(class_logits)) {
    if (candidate.label != kRinkClass)
      continue;
    const float* query_mask = mask_logits + static_cast<size_t>(candidate.query) * kMaskHeight * kMaskWidth;
    cv::Mat raw(kMaskHeight, kMaskWidth, CV_32F, const_cast<float*>(query_mask));
    cv::Mat padded;
    cv::resize(raw, padded, {kInputWidth, kInputHeight}, 0.0, 0.0, cv::INTER_LINEAR);
    cv::Mat cropped = padded(cv::Rect(0, 0, input.resized_width, input.resized_height));
    cv::Mat original;
    cv::resize(cropped, original, {input.source_width, input.source_height}, 0.0, 0.0, cv::INTER_LINEAR);

    cv::Mat binary;
    cv::compare(original, 0.0, binary, cv::CMP_GT);
    const int positive_count = cv::countNonZero(binary);
    if (positive_count == 0)
      continue;
    double mask_probability_sum = 0.0;
    for (int y = 0; y < original.rows; ++y) {
      const float* logits_row = original.ptr<float>(y);
      const uint8_t* mask_row = binary.ptr<uint8_t>(y);
      for (int x = 0; x < original.cols; ++x) {
        if (mask_row[x] != 0)
          mask_probability_sum += sigmoid(logits_row[x]);
      }
    }
    const float score = candidate.class_score * static_cast<float>(mask_probability_sum / positive_count);
    if (!(score > kScoreThreshold) || !std::isfinite(score))
      continue;
    result.masks.push_back(binary);
    result.scores.push_back(score);
  }
  if (result.masks.empty()) {
    return absl::NotFoundError("Ice-rink model produced no class-1 mask above score 0.3");
  }

  result.combined_mask = cv::Mat::zeros(input.source_height, input.source_width, CV_8U);
  bool have_bbox = false;
  double min_x = std::numeric_limits<double>::max();
  double min_y = std::numeric_limits<double>::max();
  double max_x = 0.0;
  double max_y = 0.0;
  for (const cv::Mat& mask : result.masks) {
    cv::bitwise_or(result.combined_mask, mask, result.combined_mask);
    std::vector<cv::Point> nonzero;
    cv::findNonZero(mask, nonzero);
    if (nonzero.empty())
      continue;
    const cv::Rect bbox = cv::boundingRect(nonzero);
    min_x = std::min(min_x, static_cast<double>(bbox.x));
    min_y = std::min(min_y, static_cast<double>(bbox.y));
    max_x = std::max(max_x, static_cast<double>(bbox.x + bbox.width));
    max_y = std::max(max_y, static_cast<double>(bbox.y + bbox.height));
    have_bbox = true;
  }
  if (!have_bbox)
    return absl::InternalError("Ice-rink masks were unexpectedly empty");
  result.combined_bbox = {min_x, min_y, max_x - min_x, max_y - min_y};
  result.centroid = contour_centroid(result.masks);
  auto validation = validate_profile_geometry(result, input.source_width, input.source_height);
  if (!validation.ok())
    return validation;
  return result;
}

absl::StatusOr<RinkProfile> RinkSegmentation::Infer(const cv::Mat& bgr_image, double inference_scale) const {
  if (bgr_image.empty() || bgr_image.type() != CV_8UC3) {
    return absl::InvalidArgumentError("Rink input must be a non-empty CV_8UC3 BGR image");
  }
  if (!(inference_scale > 0.0) || !std::isfinite(inference_scale)) {
    return absl::InvalidArgumentError("Rink inference scale must be finite and positive");
  }
  cv::Mat inference_image = bgr_image;
  if (inference_scale != 1.0) {
    const double scaled_width = std::round(bgr_image.cols * inference_scale);
    const double scaled_height = std::round(bgr_image.rows * inference_scale);
    if (!std::isfinite(scaled_width) || !std::isfinite(scaled_height) ||
        scaled_width > std::numeric_limits<int>::max() || scaled_height > std::numeric_limits<int>::max()) {
      return absl::InvalidArgumentError("Rink inference scale produces invalid image dimensions");
    }
    const int width = std::max(1, static_cast<int>(scaled_width));
    const int height = std::max(1, static_cast<int>(scaled_height));
    cv::resize(
        bgr_image,
        inference_image,
        {width, height},
        0.0,
        0.0,
        inference_scale < 1.0 ? cv::INTER_AREA : cv::INTER_LINEAR);
  }
  auto input = Prepare(inference_image);
  if (!input.ok())
    return input.status();
  auto outputs =
      session_->RunFloat("images", {1, 3, kInputHeight, kInputWidth}, input->tensor.data(), input->tensor.size());
  if (!outputs.ok())
    return outputs.status();
  if (outputs->size() != 2)
    return absl::InternalError("Rink ONNX model returned an unexpected output count");
  auto classes = outputs->at(0).float_data();
  auto masks = outputs->at(1).float_data();
  auto class_count = outputs->at(0).element_count();
  auto mask_count = outputs->at(1).element_count();
  if (!classes.ok())
    return classes.status();
  if (!masks.ok())
    return masks.status();
  if (!class_count.ok())
    return class_count.status();
  if (!mask_count.ok())
    return mask_count.status();
  auto profile = Postprocess(*input, *classes, *class_count, *masks, *mask_count);
  if (!profile.ok())
    return profile.status();

  if (inference_scale != 1.0) {
    for (cv::Mat& mask : profile->masks) {
      cv::resize(mask, mask, bgr_image.size(), 0.0, 0.0, cv::INTER_NEAREST);
    }
    cv::resize(profile->combined_mask, profile->combined_mask, bgr_image.size(), 0.0, 0.0, cv::INTER_NEAREST);
    profile->centroid.x /= inference_scale;
    profile->centroid.y /= inference_scale;
    profile->combined_bbox.x /= inference_scale;
    profile->combined_bbox.y /= inference_scale;
    profile->combined_bbox.width /= inference_scale;
    profile->combined_bbox.height /= inference_scale;
  }
  auto validation = validate_profile_geometry(*profile, bgr_image.cols, bgr_image.rows);
  if (!validation.ok())
    return validation;
  return profile;
}

} // namespace hm::stitching
