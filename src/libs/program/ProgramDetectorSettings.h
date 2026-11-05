#pragma once

#include <filesystem>
#include <functional>
#include <string>

#include <optional>
#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/TileInference.h"
#include "yaml-cpp/yaml.h"

namespace hm::program {
struct ProgramDetectorSettings {
  std::filesystem::path onnx;
  std::string input_name;
  int width, height, classes, maximum_detections;
  float scale, confidence_threshold, iou_threshold;
  bool maintain_aspect, symmetric_padding, fp16;
};
// Prepare the selected detector's declared assets and read its input contract.
// This is startup-only CPU metadata/file work: no inference or GPU session.
// Only the native FP32/FP16 RGB YOLO detection contract is supported. Explicit
// prebuilt TensorRT precisions fail, with no silent precision substitution.
absl::StatusOr<ProgramDetectorSettings> PrepareProgramDetector(
    const std::filesystem::path& inference_file,
    const YAML::Node& inference,
    const std::function<bool()>& cancelled,
    std::optional<detection::TileInferenceSettings> tiling = std::nullopt);
} // namespace hm::program
