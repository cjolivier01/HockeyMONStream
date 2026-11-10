#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/DetectorGpu.h"
#include "hstream/src/libs/gpu/migraphx/MigraphxSession.h"

namespace hm::detection {

struct DetectorConfig {
  std::filesystem::path onnx;
  std::string input_name;
  int width, height, classes, device, maximum_detections;
  float scale, confidence_threshold, iou_threshold;
  bool maintain_aspect, symmetric_padding;
  gpu::MigraphxSession::Precision precision;
  size_t maximum_io_bytes;
  std::optional<gpu::MigraphxCacheConfig> cache;
};

// Native AMD execution of the shipped YOLO xyxy/score/class ONNX contract.
// Frames remain on the GPU; only the filtered metadata is transferred for
// deterministic NMS and tracking. One serialized caller/stream per instance.
class HipDetector {
 public:
  static absl::StatusOr<std::unique_ptr<HipDetector>> Create(const DetectorConfig& config);
  ~HipDetector();
  HipDetector(const HipDetector&) = delete;
  HipDetector& operator=(const HipDetector&) = delete;
  // Synchronous on success and error: queued reads of the borrowed image are
  // drained before returning, so the caller may then release/recycle it.
  absl::StatusOr<std::vector<Proposal>> Detect(const Image& image, hipStream_t stream);

 private:
  struct Impl;
  explicit HipDetector(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace hm::detection
