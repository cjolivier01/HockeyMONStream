#pragma once

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include "hstream/src/libs/onnx/OnnxSession.h"

namespace hm::stitching {
struct ScoreboardProposal {
  std::array<cv::Point, 4> corners;
  double score{0};
};

// Only called while waiting for a missing selection. Downloads/verifies the
// pinned asset on demand and releases the model before returning to playback.
absl::StatusOr<std::optional<ScoreboardProposal>> SuggestScoreboard(
    const cv::Mat& bgr,
    cv::Size original,
    const std::function<bool()>& cancelled = {});

absl::StatusOr<std::optional<ScoreboardProposal>> InferScoreboard(
    const std::string& model_path,
    const cv::Mat& bgr,
    cv::Size original,
    hm::onnx::ExecutionProvider provider,
    const std::function<bool()>& cancelled = {});

absl::StatusOr<std::optional<ScoreboardProposal>> ScoreboardPostprocess(
    const float* classes,
    size_t class_count,
    const float* masks,
    size_t mask_count,
    cv::Size resized,
    cv::Size original);
} // namespace hm::stitching
