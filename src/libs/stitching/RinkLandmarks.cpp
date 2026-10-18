#include "hstream/src/libs/stitching/RinkLandmarks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numeric>

#include <opencv2/imgproc.hpp>

namespace hm::stitching {
namespace {
constexpr const char* kNames[] = {
    "Blue Line",
    "Center Ice Circle",
    "Center Line",
    "Crease",
    "Faceoff Dot",
    "Field",
    "Goal",
    "Goal Line",
    "Slot Box",
    "Trapezoid",
    "Zone Circle"};
std::vector<cv::Point2d> LineSamples(const cv::Mat& mask, cv::Size original) {
  std::vector<cv::Point> points;
  cv::findNonZero(mask, points);
  if (points.size() < 20)
    return {};
  cv::Mat data(static_cast<int>(points.size()), 2, CV_64F);
  for (size_t i = 0; i < points.size(); ++i) {
    data.at<double>(static_cast<int>(i), 0) = points[i].x;
    data.at<double>(static_cast<int>(i), 1) = points[i].y;
  }
  cv::PCA pca(data, cv::Mat(), cv::PCA::DATA_AS_ROW);
  const cv::Point2d direction(pca.eigenvectors.at<double>(0, 0), pca.eigenvectors.at<double>(0, 1));
  double lo = 1e20, hi = -1e20;
  for (const auto p : points) {
    const double along = p.x * direction.x + p.y * direction.y;
    lo = std::min(lo, along);
    hi = std::max(hi, along);
  }
  if (hi - lo < 20)
    return {};
  constexpr int bins = 24;
  std::array<cv::Point2d, bins> sums{};
  std::array<int, bins> counts{};
  for (const auto p : points) {
    const int bin =
        std::clamp(static_cast<int>((p.x * direction.x + p.y * direction.y - lo) / (hi - lo) * bins), 0, bins - 1);
    sums[bin] += cv::Point2d(p.x, p.y);
    ++counts[bin];
  }
  std::vector<cv::Point2d> result;
  for (int bin = 1; bin < bins - 1; ++bin) {
    if (counts[bin] >= 3)
      result.emplace_back(
          (sums[bin].x / counts[bin] + 0.5) * original.width / mask.cols - 0.5,
          (sums[bin].y / counts[bin] + 0.5) * original.height / mask.rows - 0.5);
  }
  return result.size() >= 2 ? result : std::vector<cv::Point2d>{};
}
} // namespace

absl::StatusOr<std::unique_ptr<RinkLandmarks>> RinkLandmarks::Create(
    const std::string& model_path,
    hm::onnx::ExecutionProvider provider) {
  auto session = hm::onnx::Session::Create(
      model_path,
      {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 3, kCanvas, kCanvas}}},
      {{"class_logits", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kQueries, kClasses}},
       {"mask_logits", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kQueries, kMaskSize, kMaskSize}}},
      false,
      provider,
      {},
      {model_path, [] { std::clog << "Rink landmark GPU memory exhausted; continuing calibration on CPU\n"; }});
  if (!session.ok())
    return session.status();
  return std::unique_ptr<RinkLandmarks>(new RinkLandmarks(std::move(*session)));
}

absl::StatusOr<std::vector<RinkLandmarkDetection>> RinkLandmarks::Infer(
    const cv::Mat& bgr,
    const std::function<bool()>& cancelled) {
  if (bgr.empty() || bgr.type() != CV_8UC3 || std::max(bgr.cols, bgr.rows) > 4096)
    return absl::InvalidArgumentError(
        "Landmark inference needs a bounded BGR calibration still (at most 4096 pixels per side)");
  if (cancelled && cancelled())
    return absl::CancelledError("Rink landmark detection stopped");
  const double scale = static_cast<double>(kResize) / std::max(bgr.cols, bgr.rows);
  cv::Mat resized;
  cv::resize(
      bgr,
      resized,
      {std::max(1, static_cast<int>(std::round(bgr.cols * scale))),
       std::max(1, static_cast<int>(std::round(bgr.rows * scale)))},
      0,
      0,
      cv::INTER_LINEAR);
  std::vector<float> input(3 * kCanvas * kCanvas, 0);
  constexpr float mean[]{123.675f, 116.28f, 103.53f};
  constexpr float deviation[]{58.395f, 57.12f, 57.375f};
  for (int row = 0; row < resized.rows; ++row)
    for (int col = 0; col < resized.cols; ++col)
      for (int channel = 0; channel < 3; ++channel)
        input[channel * kCanvas * kCanvas + row * kCanvas + col] =
            (resized.at<cv::Vec3b>(row, col)[2 - channel] - mean[channel]) / deviation[channel];
  auto output = session_->RunFloat("images", {1, 3, kCanvas, kCanvas}, input.data(), input.size(), cancelled);
  if (!output.ok())
    return output.status();
  if (output->size() != 2)
    return absl::DataLossError("Unexpected rink landmark output count");
  auto classes = output->at(0).float_data(), masks = output->at(1).float_data();
  auto class_count = output->at(0).element_count(), mask_count = output->at(1).element_count();
  if (!classes.ok() || !masks.ok() || !class_count.ok() || !mask_count.ok())
    return absl::DataLossError("Invalid rink landmark output tensors");
  return Postprocess(*classes, *class_count, *masks, *mask_count, resized.size(), bgr.size());
}

absl::StatusOr<std::vector<RinkLandmarkDetection>> RinkLandmarks::Postprocess(
    const float* classes,
    size_t class_count,
    const float* masks,
    size_t mask_count,
    cv::Size resized,
    cv::Size original) {
  if (!classes || !masks || class_count != kQueries * kClasses || mask_count != kQueries * kMaskSize * kMaskSize ||
      resized.width < 1 || resized.height < 1 || resized.width > kCanvas || resized.height > kCanvas ||
      original.width < 1 || original.height < 1 || original.width > 4096 || original.height > 4096)
    return absl::InvalidArgumentError("Rink landmark output does not match the native model contract");
  if (!std::all_of(classes, classes + class_count, [](float v) { return std::isfinite(v); }) ||
      !std::all_of(masks, masks + mask_count, [](float v) { return std::isfinite(v); }))
    return absl::DataLossError("Rink landmark output contains nonfinite values");
  struct Candidate {
    double score;
    int query;
    int label;
  };
  std::vector<Candidate> candidates;
  for (int query = 0; query < kQueries; ++query) {
    const float* logits = classes + query * kClasses;
    const float maximum = *std::max_element(logits, logits + kClasses);
    double total = 0;
    for (int label = 0; label < kClasses; ++label)
      total += std::exp(logits[label] - maximum);
    for (int label = 0; label < kClasses - 1; ++label)
      candidates.push_back({std::exp(logits[label] - maximum) / total, query, label});
  }
  std::partial_sort(candidates.begin(), candidates.begin() + 50, candidates.end(), [](const auto& a, const auto& b) {
    return a.score > b.score;
  });
  candidates.resize(50);
  std::vector<RinkLandmarkDetection> result;
  for (const auto candidate : candidates) {
    if (candidate.score < 0.5 || candidate.label == 5)
      continue;
    const cv::Mat raw(
        kMaskSize, kMaskSize, CV_32F, const_cast<float*>(masks + candidate.query * kMaskSize * kMaskSize));
    cv::Mat upsampled;
    cv::resize(raw, upsampled, {kCanvas, kCanvas}, 0, 0, cv::INTER_LINEAR);
    cv::Mat cropped = upsampled(cv::Rect({0, 0}, resized));
    cv::Mat mask = cropped > 0;
    const int area = cv::countNonZero(mask);
    if (area < 10)
      continue;
    double mask_score = 0;
    for (int row = 0; row < cropped.rows; ++row)
      for (int col = 0; col < cropped.cols; ++col)
        if (mask.at<unsigned char>(row, col))
          mask_score += 1.0 / (1.0 + std::exp(-cropped.at<float>(row, col)));
    const double score = candidate.score * mask_score / area;
    if (score < 0.5)
      continue;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty())
      continue;
    const auto largest = std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
      return cv::contourArea(a) < cv::contourArea(b);
    });
    RinkLandmarkDetection detection;
    detection.class_index = candidate.label;
    detection.label = kNames[candidate.label];
    detection.score = score;
    const size_t stride = std::max<size_t>(1, (largest->size() + 127) / 128);
    for (size_t i = 0; i < largest->size(); i += stride)
      detection.outline.emplace_back(
          ((*largest)[i].x + 0.5) * original.width / resized.width - 0.5,
          ((*largest)[i].y + 0.5) * original.height / resized.height - 0.5);
    if (candidate.label == 0 || candidate.label == 2 || candidate.label == 7)
      detection.line_samples = LineSamples(mask, original);
    result.push_back(std::move(detection));
  }
  return result;
}
} // namespace hm::stitching
