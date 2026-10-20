#include "hstream/src/libs/stitching/ScoreboardProposal.h"

#include <cuda_runtime_api.h>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <numeric>
#include "hstream/src/libs/assets/AssetManager.h"

namespace hm::stitching {
namespace {
constexpr int kWidth = 2048, kHeight = 768, kQueries = 100;
constexpr int kMaskWidth = 512, kMaskHeight = 192;
constexpr char kFilename[] = "scoreboard-mask2former-r50-3d7fc57ac7ab4513.onnx";
constexpr char kDigest[] = "3d7fc57ac7ab45139411bdb28500f518c97dda8b45a91b0d3a14c91bd3eb2665";
bool valid_size(cv::Size size) {
  return size.width > 0 && size.height > 0 && size.width <= 65535 && size.height <= 65535 &&
      static_cast<int64_t>(size.width) * size.height <= 256LL * 1024 * 1024;
}
} // namespace

absl::StatusOr<std::optional<ScoreboardProposal>> SuggestScoreboard(
    const cv::Mat& bgr,
    cv::Size original,
    const std::function<bool()>& cancelled) {
  if (cancelled && cancelled())
    return absl::CancelledError("Scoreboard detection stopped");
  std::filesystem::path path;
  if (const char* override_path = std::getenv("HM_SCOREBOARD_ONNX_MODEL"); override_path && *override_path) {
    path = override_path;
  } else {
    const char* home = std::getenv("HOME");
    if (!home || !*home)
      return absl::NotFoundError("HOME is unavailable for the scoreboard model cache");
    path = std::filesystem::path(home) / ".cache/hstream/models" / kFilename;
    hm::assets::AssetSpec spec;
    spec.name = "Scoreboard proposal";
    spec.url = "https://github.com/cjolivier01/HockeyMONStream/releases/download/pretrained-assets-v1/" +
        std::string(kFilename);
    spec.sha256 = kDigest;
    spec.target = path;
    spec.on_demand = true;
    const auto status = hm::assets::AssetManager::EnsureAsset(spec, {}, cancelled);
    if (!status.ok())
      return status;
  }
  int devices = 0;
  const auto availability = cudaGetDeviceCount(&devices);
  const auto providers = Ort::GetAvailableProviders();
  const bool has_cuda = availability == cudaSuccess && devices > 0 &&
      std::find(providers.begin(), providers.end(), "CUDAExecutionProvider") != providers.end();
  if (availability != cudaSuccess && availability != cudaErrorNoDevice && availability != cudaErrorInsufficientDriver)
    return absl::InternalError(std::string("Scoreboard CUDA device query failed: ") + cudaGetErrorString(availability));
  std::clog << "Scoreboard proposal execution provider=" << (has_cuda ? "cuda" : "cpu") << '\n';
  return InferScoreboard(
      path.string(),
      bgr,
      original,
      has_cuda ? hm::onnx::ExecutionProvider::kCuda : hm::onnx::ExecutionProvider::kCpu,
      cancelled);
}

absl::StatusOr<std::optional<ScoreboardProposal>> InferScoreboard(
    const std::string& path,
    const cv::Mat& bgr,
    cv::Size original,
    hm::onnx::ExecutionProvider provider,
    const std::function<bool()>& cancelled) {
  if (bgr.empty() || bgr.type() != CV_8UC3 || !valid_size(original) || bgr.cols > 8192 || bgr.rows > 8192)
    return absl::InvalidArgumentError("Scoreboard inference requires a bounded BGR still and valid source dimensions");
  if (cancelled && cancelled())
    return absl::CancelledError("Scoreboard detection stopped");
  const double scale = std::min({double(kWidth) / bgr.cols, double(kHeight) / bgr.rows, 1.0});
  cv::Mat resized;
  cv::resize(
      bgr,
      resized,
      {std::max(1, int(std::round(bgr.cols * scale))), std::max(1, int(std::round(bgr.rows * scale)))},
      0,
      0,
      cv::INTER_LINEAR);
  std::vector<float> input(3 * kWidth * kHeight, 0);
  constexpr float mean[]{123.675f, 116.28f, 103.53f}, deviation[]{58.395f, 57.12f, 57.375f};
  for (int y = 0; y < resized.rows; ++y)
    for (int x = 0; x < resized.cols; ++x)
      for (int channel = 0; channel < 3; ++channel)
        input[channel * kWidth * kHeight + y * kWidth + x] =
            (resized.at<cv::Vec3b>(y, x)[2 - channel] - mean[channel]) / deviation[channel];
  auto session = hm::onnx::Session::Create(
      path,
      {{"images", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, 3, kHeight, kWidth}}},
      {{"class_logits", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kQueries, 2}},
       {"mask_logits", ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {1, kQueries, kMaskHeight, kMaskWidth}}},
      false,
      provider,
      {},
      {path, [] { std::clog << "Scoreboard GPU memory exhausted; retrying once on CPU\n"; }});
  if (!session.ok())
    return session.status();
  auto output = (*session)->RunFloat("images", {1, 3, kHeight, kWidth}, input.data(), input.size(), cancelled);
  if (!output.ok())
    return output.status();
  if (output->size() != 2)
    return absl::DataLossError("Unexpected scoreboard output count");
  auto classes = output->at(0).float_data(), masks = output->at(1).float_data();
  auto nc = output->at(0).element_count(), nm = output->at(1).element_count();
  if (!classes.ok() || !masks.ok() || !nc.ok() || !nm.ok())
    return absl::DataLossError("Invalid scoreboard output tensors");
  return ScoreboardPostprocess(*classes, *nc, *masks, *nm, resized.size(), original);
}

absl::StatusOr<std::optional<ScoreboardProposal>> ScoreboardPostprocess(
    const float* classes,
    size_t nc,
    const float* masks,
    size_t nm,
    cv::Size resized,
    cv::Size original) {
  if (!classes || !masks || nc != kQueries * 2 || nm != kQueries * kMaskHeight * kMaskWidth || !valid_size(original) ||
      resized.width <= 0 || resized.width > kWidth || resized.height <= 0 || resized.height > kHeight)
    return absl::InvalidArgumentError("Scoreboard output does not match the exported model contract");
  if (!std::all_of(classes, classes + nc, [](float x) { return std::isfinite(x); }) ||
      !std::all_of(masks, masks + nm, [](float x) { return std::isfinite(x); }))
    return absl::DataLossError("Scoreboard model returned nonfinite values");
  std::array<double, kQueries> scores;
  std::array<int, kQueries> order;
  std::iota(order.begin(), order.end(), 0);
  for (int q = 0; q < kQueries; ++q)
    scores[q] = 1.0 / (1.0 + std::exp(double(classes[q * 2 + 1]) - classes[q * 2]));
  std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return scores[a] > scores[b]; });
  double best = 0.5;
  std::optional<ScoreboardProposal> result;
  for (int rank = 0; rank < 10; ++rank) {
    const int q = order[rank];
    if (scores[q] < best)
      continue;
    const cv::Mat raw(kMaskHeight, kMaskWidth, CV_32F, const_cast<float*>(masks + q * kMaskHeight * kMaskWidth));
    cv::Mat enlarged;
    cv::resize(raw, enlarged, {kWidth, kHeight}, 0, 0, cv::INTER_LINEAR);
    const cv::Mat cropped = enlarged(cv::Rect({0, 0}, resized));
    cv::Mat mask = cropped > 0;
    const int area = cv::countNonZero(mask);
    if (!area)
      continue;
    double confidence = 0;
    for (int y = 0; y < cropped.rows; ++y)
      for (int x = 0; x < cropped.cols; ++x)
        if (mask.at<unsigned char>(y, x))
          confidence += 1.0 / (1.0 + std::exp(-cropped.at<float>(y, x)));
    confidence *= scores[q] / area;
    if (confidence < best)
      continue;
    std::vector<std::vector<cv::Point>> contours;
    cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
    if (contours.empty())
      continue;
    auto largest = std::max_element(contours.begin(), contours.end(), [](const auto& a, const auto& b) {
      return cv::contourArea(a) < cv::contourArea(b);
    });
    std::vector<cv::Point> hull, approximate;
    cv::convexHull(*largest, hull);
    if (cv::contourArea(hull) <= 0)
      continue;
    const double perimeter = cv::arcLength(hull, true);
    std::array<cv::Point2f, 4> quad;
    bool fitted = false;
    for (int step = 0; step < 28; ++step) {
      cv::approxPolyDP(hull, approximate, (0.01 + step * .005) * perimeter, true);
      if (approximate.size() == 4) {
        std::copy(approximate.begin(), approximate.end(), quad.begin());
        fitted = true;
        break;
      }
    }
    if (!fitted) {
      std::clog << "Scoreboard mask has no four-corner fit; using its minimum-area rectangle\n";
      cv::minAreaRect(hull).points(quad.data());
    }
    cv::Point2f centre;
    for (auto p : quad)
      centre += p * .25f;
    std::sort(quad.begin(), quad.end(), [&](auto a, auto b) {
      return std::atan2(a.y - centre.y, a.x - centre.x) < std::atan2(b.y - centre.y, b.x - centre.x);
    });
    auto first = std::min_element(quad.begin(), quad.end(), [](auto a, auto b) { return a.x + a.y < b.x + b.y; });
    std::rotate(quad.begin(), first, quad.end());
    ScoreboardProposal proposal;
    proposal.score = confidence;
    for (size_t i = 0; i < 4; ++i)
      proposal.corners[i] = {
          std::clamp(int(std::round(quad[i].x * original.width / resized.width)), 0, original.width - 1),
          std::clamp(int(std::round(quad[i].y * original.height / resized.height)), 0, original.height - 1)};
    if (!cv::isContourConvex(std::vector<cv::Point>(proposal.corners.begin(), proposal.corners.end())))
      continue;
    best = confidence;
    result = proposal;
  }
  return result;
}
} // namespace hm::stitching
