#include "hstream/src/libs/stitching/ScoreboardProposal.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <chrono>
#include <iostream>
#include <limits>

namespace {
bool check(bool value, const char* message) {
  if (!value)
    std::cerr << message << '\n';
  return value;
}
} // namespace

int main(int argc, char** argv) {
  // Optional real-model parity/performance check: path image [cpu|cuda].
  if (argc == 4) {
    auto provider = hm::onnx::ParseExecutionProvider(argv[3]);
    if (!provider.ok())
      return 1;
    cv::Mat source = cv::imread(argv[2]);
    if (source.empty())
      return 1;
    const auto original = source.size();
    const double scale = std::min(2048.0 / source.cols, 768.0 / source.rows);
    cv::resize(source, source, {}, scale, scale, cv::INTER_LINEAR);
    const auto start = std::chrono::steady_clock::now();
    const auto result = hm::stitching::InferScoreboard(argv[1], source, original, *provider);
    if (!result.ok()) {
      std::cerr << result.status() << '\n';
      return 1;
    }
    if (!result->has_value()) {
      std::cerr << "No proposal\n";
      return 1;
    }
    std::cout << "score=" << (**result).score << " corners=";
    for (auto point : (**result).corners)
      std::cout << point << ' ';
    std::cout << " seconds=" << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() << '\n';
    return 0;
  }
  std::vector<float> classes(200, -20), masks(100 * 192 * 512, -20);
  for (int q = 0; q < 100; ++q)
    classes[q * 2 + 1] = 20;
  auto run = [&] {
    return hm::stitching::ScoreboardPostprocess(
        classes.data(), classes.size(), masks.data(), masks.size(), {1024, 384}, {8192, 3072});
  };
  auto result = run();
  bool ok = check(result.ok() && !result->has_value(), "Background must not become a scoreboard proposal");
  classes[0] = 20;
  classes[1] = -20;
  for (int y = 10; y < 20; ++y)
    for (int x = 30; x < 50; ++x)
      masks[y * 512 + x] = 20;
  result = run();
  ok &= check(result.ok() && result->has_value(), "Confident mask must become a proposal");
  if (result.ok() && result->has_value()) {
    const std::array<cv::Point, 4> expected{{{960, 320}, {1592, 320}, {1592, 632}, {960, 632}}};
    for (size_t i = 0; i < 4; ++i)
      ok &= check(
          cv::norm((**result).corners[i] - expected[i]) <= 23,
          "Corners must scale to the source, clockwise from top left");
  }
  masks[0] = std::numeric_limits<float>::quiet_NaN();
  ok &= check(!run().ok(), "Nonfinite output must fail");
  ok &= check(
      !hm::stitching::ScoreboardPostprocess(classes.data(), 1, masks.data(), masks.size(), {1024, 384}, {8192, 3072})
           .ok(),
      "Wrong output shape must fail");
  const auto cancelled = hm::stitching::InferScoreboard(
      "not-opened.onnx", cv::Mat(4, 4, CV_8UC3), {4, 4}, hm::onnx::ExecutionProvider::kCpu, [] { return true; });
  ok &= check(absl::IsCancelled(cancelled.status()), "Cancellation must skip model loading");
  return ok ? 0 : 1;
}
