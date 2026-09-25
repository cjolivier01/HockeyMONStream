#include "hstream/src/libs/player_analytics/ModelContract.h"

#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <string_view>

#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {
namespace {

void Require(bool condition, const char* message) {
  if (!condition)
    throw std::invalid_argument(message);
}

std::string String(const YAML::Node& node, const char* key, size_t maximum = 256) {
  const auto value = node[key];
  Require(value && value.IsScalar(), "required manifest string is missing or not scalar");
  auto result = value.as<std::string>();
  Require(
      !result.empty() && result.size() <= maximum && result.find('\0') == std::string::npos,
      "manifest string is empty, too long, or contains NUL");
  return result;
}

void Keys(const YAML::Node& node, std::initializer_list<std::string_view> allowed) {
  Require(node.IsMap(), "manifest section must be a mapping");
  std::set<std::string> seen;
  for (const auto& item : node) {
    const auto key = item.first.as<std::string>();
    Require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), "unrecognized manifest key");
    Require(seen.insert(key).second, "duplicate manifest key");
  }
}

std::string Digest(const YAML::Node& node, const char* key) {
  auto digest = String(node, key, 64);
  Require(
      digest.size() == 64 &&
          std::all_of(
              digest.begin(), digest.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }),
      "SHA256 must contain 64 lowercase hexadecimal characters");
  return digest;
}

std::string RelativeFile(const YAML::Node& node) {
  const auto file = String(node, "file", 4096);
  Require(file.front() != '/' && file.find('\\') == std::string::npos, "bundle file must be a relative POSIX path");
  size_t begin = 0;
  for (;;) {
    const auto end = file.find('/', begin);
    const auto part = file.substr(begin, end == std::string::npos ? end : end - begin);
    Require(!part.empty() && part != "." && part != "..", "bundle file path contains an unsafe component");
    if (end == std::string::npos)
      break;
    begin = end + 1;
  }
  return file;
}

void Number(const YAML::Node& node, const char* key, double expected) {
  const double actual = node[key].as<double>();
  Require(std::isfinite(actual) && std::abs(actual - expected) <= 0.00001, "unsupported preprocessing constant");
}

void Numbers(const YAML::Node& node, const char* key, std::initializer_list<double> expected) {
  const auto values = node[key];
  Require(values.IsSequence() && values.size() == expected.size(), "wrong preprocessing array shape");
  size_t i = 0;
  for (double value : expected) {
    const double actual = values[i++].as<double>();
    Require(std::isfinite(actual) && std::abs(actual - value) <= 0.00001, "unsupported preprocessing array value");
  }
}

std::vector<TensorContract> Tensors(const YAML::Node& node, const std::vector<std::vector<int64_t>>& shapes) {
  Require(node.IsSequence() && node.size() == shapes.size(), "wrong number of model tensors");
  std::vector<TensorContract> result;
  for (size_t i = 0; i < shapes.size(); ++i) {
    Keys(node[i], {"name", "dtype", "shape"});
    TensorContract tensor;
    tensor.name = String(node[i], "name");
    tensor.dtype = String(node[i], "dtype");
    Require(tensor.dtype == "float32", "deployment bindings must be float32 (internal precision may be fp16)");
    const auto dimensions = node[i]["shape"];
    Require(dimensions.IsSequence() && dimensions.size() == shapes[i].size(), "wrong tensor rank");
    for (size_t d = 0; d < dimensions.size(); ++d)
      tensor.shape.push_back(dimensions[d].as<int64_t>());
    Require(tensor.shape == shapes[i], "tensor shape does not match the fixed deployment profile");
    result.push_back(std::move(tensor));
  }
  return result;
}

std::vector<std::string> Strings(const YAML::Node& node, size_t count, size_t maximum_length) {
  Require(node.IsSequence() && node.size() == count, "model dictionary has the wrong size");
  std::set<std::string> seen;
  std::vector<std::string> result;
  for (const auto& item : node) {
    Require(item.IsScalar(), "dictionary entries must be scalar strings");
    const auto text = item.as<std::string>();
    Require(
        !text.empty() && text.size() <= maximum_length && text.find('\0') == std::string::npos,
        "model dictionary contains an invalid string");
    Require(seen.insert(text).second, "model dictionary contains duplicate entries");
    result.push_back(text);
  }
  return result;
}

void PoseProfile(const YAML::Node& node, ModelManifest* manifest) {
  Require(manifest->model_family == "rtmpose", "pose model family must be rtmpose");
  manifest->topology_id = String(node, "topology_id");
  Require(manifest->topology_id == "coco17", "only coco17 pose layout is supported");
  manifest->inputs = Tensors(node["inputs"], {{-1, 3, 256, 192}});
  manifest->outputs = Tensors(node["outputs"], {{-1, 17, 384}, {-1, 17, 512}});
  const auto p = node["preprocessing"];
  Keys(p, {"id", "color", "mean", "std", "padding", "simcc_split_ratio", "flip_test"});
  manifest->preprocessing_id = String(p, "id");
  Require(manifest->preprocessing_id == "rtmpose-coco17-affine-v1", "unsupported pose preprocessing");
  Require(String(p, "color") == "rgb", "pose color order must be rgb");
  Numbers(p, "mean", {123.675, 116.28, 103.53});
  Numbers(p, "std", {58.395, 57.12, 57.375});
  Number(p, "padding", 1.25);
  Number(p, "simcc_split_ratio", 2);
  Require(!p["flip_test"].as<bool>(), "pose flip test is unsupported by this deployment profile");
}

void JerseyProfile(const YAML::Node& node, ModelManifest* manifest) {
  Require(manifest->model_family == "parseq", "jersey model family must be parseq");
  manifest->inputs = Tensors(node["inputs"], {{-1, 3, 32, 128}});
  manifest->outputs = Tensors(node["outputs"], {{-1, 3, 95}});
  const auto p = node["preprocessing"];
  Keys(
      p,
      {"id",
       "color",
       "input_range",
       "interpolation",
       "align_corners",
       "antialias",
       "cubic_coefficient",
       "resize_coordinate_mode",
       "quantize_uint8_after_resize",
       "max_length",
       "eos_index",
       "charset"});
  manifest->preprocessing_id = String(p, "id");
  Require(manifest->preprocessing_id == "parseq-rgb-bicubic-v1", "unsupported jersey preprocessing");
  Require(
      String(p, "color") == "rgb" && String(p, "interpolation") == "bicubic" &&
          String(p, "resize_coordinate_mode") == "half_pixel",
      "unsupported jersey crop/color convention");
  Numbers(p, "input_range", {-1, 1});
  Number(p, "cubic_coefficient", -0.5);
  Number(p, "max_length", 2);
  Require(
      !p["align_corners"].as<bool>() && p["antialias"].as<bool>() && p["quantize_uint8_after_resize"].as<bool>(),
      "jersey resize must match PIL uint8 bicubic");
  manifest->charset = Strings(p["charset"], 95, 8);
  const int64_t eos = p["eos_index"].as<int64_t>();
  Require(eos >= 0 && eos < static_cast<int64_t>(manifest->charset.size()), "EOS index is out of bounds");
  manifest->eos_index = static_cast<size_t>(eos);
  Require(manifest->charset[manifest->eos_index] == "[E]", "EOS index does not bind the actual EOS token");
  for (size_t i = 0; i < manifest->charset.size(); ++i)
    if (i != manifest->eos_index)
      Require(
          manifest->charset[i].size() == 1 && manifest->charset[i][0] >= '!' && manifest->charset[i][0] <= '~',
          "PARSeq profile requires the actual 94 printable non-space output characters");
  for (char digit = '0'; digit <= '9'; ++digit)
    Require(
        std::find(manifest->charset.begin(), manifest->charset.end(), std::string(1, digit)) != manifest->charset.end(),
        "PARSeq output vocabulary is missing a digit");
}

void ActionProfile(const YAML::Node& node, ModelManifest* manifest) {
  Require(manifest->model_family == "stgcnpp", "action model family must be stgcnpp");
  manifest->topology_id = String(node, "topology_id");
  Require(manifest->topology_id == "coco17", "action model requires coco17 keypoints");
  manifest->inputs = Tensors(node["inputs"], {{-1, 2, 100, 17, 3}});
  manifest->outputs = Tensors(node["outputs"], {{-1, 60}});
  manifest->labels = Strings(node["labels"], 60, 128);
  const auto p = node["preprocessing"];
  Keys(p, {"id", "coordinate_normalization", "sample_period_ns", "max_gap_ns", "samples", "second_person"});
  manifest->preprocessing_id = String(p, "id");
  Require(manifest->preprocessing_id == "stgcnpp-coco17-causal-v1", "unsupported action preprocessing");
  Require(
      String(p, "coordinate_normalization") == "full_canvas_minus1_plus1" && String(p, "second_person") == "zeros",
      "unsupported action normalization/person profile");
  Require(
      p["sample_period_ns"].as<uint64_t>() == kActionSamplePeriod &&
          p["max_gap_ns"].as<uint64_t>() == kActionMaximumGap && p["samples"].as<size_t>() == kActionSamples,
      "unsupported causal action timeline profile");
}

} // namespace

absl::StatusOr<ModelManifest> ParseModelManifest(const YAML::Node& node) {
  try {
    Keys(
        node,
        {"schema_version",
         "feature",
         "model_family",
         "model_id",
         "source",
         "onnx",
         "engine",
         "inputs",
         "outputs",
         "preprocessing",
         "topology_id",
         "labels"});
    ModelManifest manifest;
    Require(node["schema_version"].as<uint32_t>() == 1, "unsupported model manifest schema");
    manifest.model_id = String(node, "model_id");
    manifest.model_family = String(node, "model_family");
    const auto feature = String(node, "feature");
    if (feature == "pose")
      manifest.feature = ModelFeature::kPose;
    else if (feature == "jersey")
      manifest.feature = ModelFeature::kJersey;
    else if (feature == "action")
      manifest.feature = ModelFeature::kAction;
    else
      throw std::invalid_argument("unknown model feature");
    const auto source = node["source"];
    Keys(source, {"url", "sha256", "config_sha256", "license"});
    manifest.source_url = String(source, "url", 4096);
    manifest.source_sha256 = Digest(source, "sha256");
    manifest.source_config_sha256 = Digest(source, "config_sha256");
    manifest.license = String(source, "license", 4096);
    const auto onnx = node["onnx"];
    Keys(onnx, {"file", "sha256", "opset"});
    manifest.onnx_file = RelativeFile(onnx);
    manifest.onnx_sha256 = Digest(onnx, "sha256");
    manifest.onnx_opset = onnx["opset"].as<int>();
    Require(manifest.onnx_opset >= 13 && manifest.onnx_opset <= 21, "unsupported ONNX opset");
    const auto engine = node["engine"];
    Keys(
        engine,
        {"file",
         "sha256",
         "tensorrt_version",
         "tensorrt_build_version",
         "cuda_runtime_version",
         "gpu_name",
         "compute_capability",
         "precision",
         "max_batch"});
    manifest.engine_file = RelativeFile(engine);
    Require(manifest.engine_file != manifest.onnx_file, "engine and ONNX files must be different");
    manifest.engine_sha256 = Digest(engine, "sha256");
    manifest.engine_identity.tensorrt_version = String(engine, "tensorrt_version");
    manifest.engine_identity.tensorrt_build_version = engine["tensorrt_build_version"].as<int>();
    manifest.engine_identity.cuda_runtime_version = engine["cuda_runtime_version"].as<int>();
    manifest.engine_identity.gpu_name = String(engine, "gpu_name");
    manifest.engine_identity.compute_capability = String(engine, "compute_capability");
    Require(
        manifest.engine_identity.tensorrt_build_version >= 0 && manifest.engine_identity.cuda_runtime_version > 0,
        "invalid native SDK version identity");
    manifest.precision = String(engine, "precision");
    Require(manifest.precision == "fp16" || manifest.precision == "fp32", "unsupported engine precision");
    const auto batch = engine["max_batch"].as<int64_t>();
    Require(batch > 0 && batch <= static_cast<int64_t>(kMaximumBatch), "engine batch exceeds hard cap");
    manifest.maximum_batch = batch;
    switch (manifest.feature) {
      case ModelFeature::kPose:
        PoseProfile(node, &manifest);
        break;
      case ModelFeature::kJersey:
        JerseyProfile(node, &manifest);
        break;
      case ModelFeature::kAction:
        ActionProfile(node, &manifest);
        break;
    }
    std::set<std::string> names;
    for (const auto* tensors : {&manifest.inputs, &manifest.outputs})
      for (const auto& tensor : *tensors)
        Require(names.insert(tensor.name).second, "tensor binding names must be unique");
    return manifest;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("player analytics model manifest: ") + error.what());
  }
}

absl::Status ValidateRuntimeIdentity(const ModelManifest& manifest, const RuntimeIdentity& runtime) {
  const auto& prepared = manifest.engine_identity;
  if (prepared.tensorrt_version != runtime.tensorrt_version ||
      prepared.tensorrt_build_version != runtime.tensorrt_build_version ||
      prepared.cuda_runtime_version != runtime.cuda_runtime_version || prepared.gpu_name != runtime.gpu_name ||
      prepared.compute_capability != runtime.compute_capability)
    return absl::FailedPreconditionError("prepared engine SDK/GPU identity does not match the loaded runtime");
  return absl::OkStatus();
}

} // namespace hm::player_analytics
