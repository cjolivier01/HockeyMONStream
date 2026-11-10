#include "hstream/src/libs/program/ProgramDetectorSettings.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>

#include "hstream/src/libs/assets/AssetManager.h"
#include "hstream/src/libs/onnx/ImageInput.h"

namespace hm::program {
namespace {
void Require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
std::filesystem::path ModelPath(const std::string& text, const std::filesystem::path& directory) {
  Require(!text.empty(), "Detector ONNX path is empty");
  std::string result;
  size_t index = 0;
  const auto expand = [&](const std::string& name) {
    const auto* value = std::getenv(name.c_str());
    Require(value && *value, "Detector path references an unset/empty environment variable");
    result += value;
  };
  if (text[0] == '~' && (text.size() == 1 || text[1] == '/')) {
    expand("HOME");
    index = 1;
  }
  while (index < text.size()) {
    if (text[index] != '$') {
      result += text[index++];
      continue;
    }
    size_t begin = ++index;
    if (index < text.size() && text[index] == '{') {
      begin = ++index;
      index = text.find('}', begin);
      Require(index != std::string::npos && index > begin, "Malformed detector path environment reference");
      expand(text.substr(begin, index - begin));
      ++index;
    } else {
      while (index < text.size() && (std::isalnum(static_cast<unsigned char>(text[index])) || text[index] == '_'))
        ++index;
      Require(index > begin, "Malformed detector path environment reference");
      expand(text.substr(begin, index - begin));
    }
  }
  Require(result.find('\0') == std::string::npos, "Detector path contains a NUL byte");
  const std::filesystem::path path(result);
  return std::filesystem::weakly_canonical(
      std::filesystem::absolute(path.is_absolute() ? path : directory / path).lexically_normal());
}
} // namespace
absl::StatusOr<ProgramDetectorSettings> PrepareProgramDetector(
    const std::filesystem::path& inference_file,
    const YAML::Node& inference,
    const std::function<bool()>& cancelled) {
  if (cancelled && cancelled())
    return absl::CancelledError("Detector preparation cancelled");
  try {
    Require(
        inference.IsMap() && inference["property"].IsMap() && inference["class-attrs-all"].IsMap(),
        "Detector YAML requires property and class-attrs-all maps");
    Require(!inference["hstream-prebuilt-precision"], "AMD does not load prebuilt TensorRT precisions");
    const auto property = inference["property"], attrs = inference["class-attrs-all"];
    for (const char* key : {"offsets", "mean-file", "infer-dims", "uff-input-dims"})
      Require(!property[key] || property[key].IsNull(), "AMD Program does not support mean/offset or shape overrides");
    for (const char* key : {"input-tensor-meta", "network-input-order"})
      Require(!property[key] || property[key].as<int>() == 0, "AMD Program requires native NCHW preprocessing");
    Require(
        !property["filter-out-class-ids"] || property["filter-out-class-ids"].IsNull(),
        "AMD Program does not support detector class exclusions");
    for (const char* key :
         {"detected-min-w",
          "detected-min-h",
          "detected-max-w",
          "detected-max-h",
          "roi-top-offset",
          "roi-bottom-offset"})
      Require(!attrs[key] || attrs[key].as<int>() == 0, "AMD Program does not support detector size/ROI filters");
    Require(
        !attrs["post-cluster-threshold"] || attrs["post-cluster-threshold"].as<float>() == 0,
        "AMD Program does not support post-cluster score filtering");
    for (const auto& entry : inference) {
      const auto name = entry.first.as<std::string>();
      Require(
          name == "class-attrs-all" || name.rfind("class-attrs-", 0) != 0,
          "AMD Program does not support per-class detector overrides");
    }
    Require(
        property["model-color-format"].as<int>() == 0 && property["network-type"].as<int>() == 0 &&
            property["process-mode"].as<int>() == 1 && property["cluster-mode"].as<int>() == 2,
        "AMD Program requires full-frame RGB detection with NMS");
    Require(property["interval"].as<int>() == 0, "AMD Program currently requires inference on every frame");
    const int precision = property["network-mode"].as<int>();
    Require(precision == 0 || precision == 2, "AMD Program supports FP32 or FP16 detection");
    const int aspect = property["maintain-aspect-ratio"].as<int>(), padding = property["symmetric-padding"].as<int>();
    Require((aspect == 0 || aspect == 1) && (padding == 0 || padding == 1), "Invalid detector padding flags");
    const auto origin = std::filesystem::weakly_canonical(std::filesystem::absolute(inference_file));
    ProgramDetectorSettings result{
        ModelPath(property["onnx-file"].as<std::string>(), origin.parent_path()),
        {},
        0,
        0,
        property["num-detected-classes"].as<int>(),
        attrs["topk"].as<int>(),
        property["net-scale-factor"].as<float>(),
        attrs["pre-cluster-threshold"].as<float>(),
        attrs["nms-iou-threshold"].as<float>(),
        aspect != 0,
        padding != 0,
        precision == 2};
    Require(
        result.classes > 0 && result.maximum_detections > 0 && std::isfinite(result.scale) && result.scale > 0 &&
            std::isfinite(result.confidence_threshold) && result.confidence_threshold >= 0 &&
            result.confidence_threshold <= 1 && std::isfinite(result.iou_threshold) && result.iou_threshold >= 0 &&
            result.iou_threshold <= 1,
        "Invalid detector class count, scale or NMS settings");
    // Discover from the exact in-memory document used above, not a reopened
    // YAML that could select another model between configuration and startup.
    auto declarations = assets::AssetManager::DeclaredAssets(inference, origin);
    if (!declarations.ok())
      return declarations.status();
    for (const auto& asset : *declarations) {
      if (std::filesystem::absolute(asset.target).lexically_normal() == result.onnx) {
        const auto status = assets::AssetManager::EnsureAsset(asset, {}, cancelled);
        if (!status.ok())
          return status;
      }
    }
    if (cancelled && cancelled())
      return absl::CancelledError("Detector preparation cancelled");
    const auto input = onnx::ReadImageInput(result.onnx);
    if (!input.ok())
      return input.status();
    result.input_name = input->name;
    result.width = input->width;
    result.height = input->height;
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Program detector preparation: " + std::string(error.what()));
  }
}
} // namespace hm::program
