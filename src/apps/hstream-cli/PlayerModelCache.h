#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "absl/status/status.h"
#include "yaml-cpp/yaml.h"

namespace hm::player_analytics {
struct CatalogModel;
}

namespace hm::pipeline {

struct PlayerModelCacheOptions {
  std::function<bool()> cancelled;
  std::function<void(std::string)> progress;
  // Optional embedding/test overrides. Ordinary playback discovers the native
  // helper beside the executable and uses the standard user cache directory.
  std::filesystem::path builder_path;
  std::filesystem::path cache_root;
  std::function<const hm::player_analytics::CatalogModel*(std::string_view, std::string_view)> model_lookup;
};

// Resolve enabled built-in selections into verified immutable GPU-specific
// bundles before native config parsing. Disabled and custom selections perform
// no preparation. Enabled analytics/ReID publish their resolved GPUs for alternate
// runtimes, including custom selections. The caller owns cancellation signal
// handling. YAML paths/devices change only after all preparation succeeds.
absl::Status PreparePlayerModelCache(
    YAML::Node pipeline,
    const std::filesystem::path& config_directory,
    const PlayerModelCacheOptions& options = {});

} // namespace hm::pipeline
