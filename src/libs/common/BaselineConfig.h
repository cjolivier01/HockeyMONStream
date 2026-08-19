#pragma once

#include <filesystem>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::baseline_config {

inline constexpr char kBaselineFilename[] = "baseline.yaml";

struct BaselineConfig {
  std::filesystem::path root;
  std::filesystem::path path;
  YAML::Node values;
};

// Resolves HStream's configuration directory. HM_CONFIG_ROOT is an explicit
// override: when set, an invalid directory is reported instead of falling
// through to a different baseline.
absl::StatusOr<std::filesystem::path> resolve_root();

// Loads and validates the complete baseline document from an already-resolved
// configuration directory.
absl::StatusOr<BaselineConfig> load_from_root(const std::filesystem::path& root);

// Resolves and loads the baseline used by this process.
absl::StatusOr<BaselineConfig> load();

} // namespace hm::baseline_config
