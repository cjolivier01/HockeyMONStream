// Offline hardware qualification only. Reads one bounded image, uploads once,
// and writes detection metadata; production video never uses this CPU path.
#include "hstream/src/libs/detection/HipDetector.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include "yaml-cpp/yaml.h"

namespace {
void Hip(hipError_t status) {
  if (status != hipSuccess)
    throw std::runtime_error(hipGetErrorString(status));
}
struct Resources {
  hipStream_t stream{};
  void* image{};
  ~Resources() {
    if (stream)
      (void)hipStreamSynchronize(stream);
    if (image)
      (void)hipFree(image);
    if (stream)
      (void)hipStreamDestroy(stream);
  }
};
} // namespace
int main(int argc, char** argv) {
  if (argc != 10) {
    std::cerr << "Usage: hip_detector_fixture MODEL IMAGE INFER_YAML fp32|fp16 WIDTH HEIGHT IO_BYTES OUTPUT_CSV RUNS\n";
    return 2;
  }
  try {
    const auto yaml = YAML::LoadFile(argv[3]);
    const auto property = yaml["property"], attrs = yaml["class-attrs-all"];
    if (property["model-color-format"].as<int>() != 0)
      throw std::runtime_error("Fixture requires RGB model");
    const std::string precision(argv[4]);
    if (precision != "fp32" && precision != "fp16")
      throw std::runtime_error("Invalid precision");
    hm::detection::DetectorConfig config{
        argv[1],
        "input",
        std::stoi(argv[5]),
        std::stoi(argv[6]),
        property["num-detected-classes"].as<int>(),
        property["gpu-id"].as<int>(),
        attrs["topk"].as<int>(),
        property["net-scale-factor"].as<float>(),
        attrs["pre-cluster-threshold"].as<float>(),
        attrs["nms-iou-threshold"].as<float>(),
        property["maintain-aspect-ratio"].as<int>() != 0,
        property["symmetric-padding"].as<int>() != 0,
        precision == "fp16" ? hm::gpu::MigraphxSession::Precision::kFloat16
                            : hm::gpu::MigraphxSession::Precision::kFloat32,
        std::stoull(argv[7])};
    const int runs = std::stoi(argv[9]);
    if (runs <= 0)
      throw std::runtime_error("Positive run count required");
    auto bgr = cv::imread(argv[2]);
    if (bgr.empty())
      throw std::runtime_error("Could not read fixture image");
    cv::Mat rgba;
    cv::cvtColor(bgr, rgba, cv::COLOR_BGR2RGBA);
    Hip(hipSetDevice(config.device));
    Resources resources;
    Hip(hipStreamCreateWithFlags(&resources.stream, hipStreamNonBlocking));
    Hip(hipMalloc(&resources.image, rgba.step * rgba.rows));
    Hip(hipMemcpyAsync(resources.image, rgba.data, rgba.step * rgba.rows, hipMemcpyHostToDevice, resources.stream));
    Hip(hipStreamSynchronize(resources.stream));
    auto detector = hm::detection::HipDetector::Create(config);
    if (!detector.ok())
      throw std::runtime_error(detector.status().ToString());
    hm::detection::Image image{
        resources.image,
        nullptr,
        rgba.step,
        0,
        rgba.cols,
        rgba.rows,
        hm::detection::PixelFormat::kRgba8,
        0,
        0,
        0,
        0,
        0,
        0,
        0};
    std::ofstream output(argv[8]);
    if (!output)
      throw std::runtime_error("Could not write fixture output");
    output << "run,left,top,right,bottom,confidence,class_id\n" << std::setprecision(9);
    for (int run = 0; run < runs; ++run) {
      const auto begin = std::chrono::steady_clock::now();
      const auto result = (*detector)->Detect(image, resources.stream);
      if (!result.ok())
        throw std::runtime_error(result.status().ToString());
      const auto duration = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
      std::cout << "Run " << run << ": " << result->size() << " detections, " << duration << " ms\n" << std::flush;
      for (const auto& p : *result)
        output << run << ',' << p.left << ',' << p.top << ',' << p.right << ',' << p.bottom << ',' << p.confidence
               << ',' << p.class_id << '\n';
    }
    if (!output)
      throw std::runtime_error("Could not write detection metadata");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
