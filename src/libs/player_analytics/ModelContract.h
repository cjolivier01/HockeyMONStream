#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::player_analytics {

enum class ModelFeature { kPose, kJersey, kAction };

struct RuntimeIdentity {
  // Actual loaded SDK version queries, never compiler header versions.
  std::string tensorrt_version;
  int tensorrt_build_version{0};
  int cuda_runtime_version{0};
  std::string gpu_name;
  std::string compute_capability;
};

struct TensorContract {
  std::string name;
  std::string dtype;
  std::vector<int64_t> shape;
};

struct ModelManifest {
  uint32_t schema_version{1};
  ModelFeature feature{ModelFeature::kPose};
  std::string model_family;
  std::string model_id;
  std::string source_url;
  std::string source_sha256;
  std::string source_config_sha256;
  std::string license;
  std::string onnx_file;
  std::string onnx_sha256;
  int onnx_opset{0};
  std::string engine_file;
  std::string engine_sha256;
  RuntimeIdentity engine_identity;
  std::string precision;
  size_t maximum_batch{0};
  std::vector<TensorContract> inputs;
  std::vector<TensorContract> outputs;
  std::string preprocessing_id;
  std::string topology_id;
  std::vector<std::string> charset;
  size_t eos_index{0};
  std::vector<std::string> labels;
};

// Validates the three supported fixed deployment profiles, including numeric
// preprocessing constants, token/layout maps, bounded shapes and SDK identity.
// Does not open files, initialize CUDA or load any inference runtime.
absl::StatusOr<ModelManifest> ParseModelManifest(const YAML::Node& node);
absl::Status ValidateRuntimeIdentity(const ModelManifest& manifest, const RuntimeIdentity& runtime);

} // namespace hm::player_analytics
