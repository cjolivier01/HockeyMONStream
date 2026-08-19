#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "yaml-cpp/node/node.h"

namespace hm::assets {

namespace internal {
absl::Status fsync_asset_parent_directory(const std::filesystem::path& target);
std::string github_token(std::chrono::milliseconds cli_timeout = std::chrono::seconds(5));
} // namespace internal

struct AssetSpec {
  std::string name;
  std::string url;
  std::string sha256;
  std::filesystem::path target;
  std::filesystem::path declaring_config;
};

struct Limits {
  size_t maximum_configs{128};
  size_t maximum_assets{128};
  size_t maximum_recursion_depth{16};
  size_t maximum_asset_bytes{1024ULL * 1024 * 1024};
  size_t maximum_total_bytes{4ULL * 1024 * 1024 * 1024};
};

class AssetManager {
 public:
  using ConfigTransform = std::function<void(YAML::Node)>;

  static absl::StatusOr<std::vector<AssetSpec>> Discover(
      const std::vector<std::filesystem::path>& configs,
      const Limits& limits = {});
  // Applies a runtime-mode transform before collecting enabled child configs.
  // Direct assets in each visited config remain available to the transformed
  // graph, while assets reachable only through disabled sections are omitted.
  static absl::StatusOr<std::vector<AssetSpec>> Discover(
      const std::vector<std::filesystem::path>& configs,
      const ConfigTransform& transform,
      const Limits& limits = {});
  static absl::Status Ensure(const std::vector<std::filesystem::path>& configs, const Limits& limits = {});
  static absl::Status Ensure(
      const std::vector<std::filesystem::path>& configs,
      const ConfigTransform& transform,
      const Limits& limits = {});
  static absl::Status Verify(const std::vector<std::filesystem::path>& configs, const Limits& limits = {});
  static absl::StatusOr<std::string> Sha256(const std::filesystem::path& path);
  static absl::StatusOr<std::string> Sha256Bytes(std::string_view contents);
};

} // namespace hm::assets
