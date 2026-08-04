#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::assets {

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
  static absl::StatusOr<std::vector<AssetSpec>> Discover(
      const std::vector<std::filesystem::path>& configs,
      const Limits& limits = {});
  static absl::Status Ensure(
      const std::vector<std::filesystem::path>& configs,
      const Limits& limits = {});
  static absl::StatusOr<std::string> Sha256(const std::filesystem::path& path);
};

}  // namespace hm::assets
