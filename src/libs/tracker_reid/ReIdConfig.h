#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "absl/status/statusor.h"

namespace hm::tracker_reid {

struct Options {
  bool tracker_enabled{false};
  bool reid_enabled{false};
  std::string tracker_library;
  std::string base_config;
  std::string overlay_config;
  std::string sub_batches;
};

// Owns only the generated YAML and its private directory. Keep this owner until
// the native tracker has been destroyed; source configs and engines are borrowed.
class PreparedConfig {
 public:
  explicit PreparedConfig(std::filesystem::path directory);
  ~PreparedConfig();
  PreparedConfig(const PreparedConfig&) = delete;
  PreparedConfig& operator=(const PreparedConfig&) = delete;

  const std::filesystem::path& path() const {
    return path_;
  }

 private:
  std::filesystem::path directory_;
  std::filesystem::path path_;
  std::filesystem::path temporary_path_;
};

// Returns a null owner without any file or environment access when either flag
// is false. Enabled preparation validates before creating any runtime files.
absl::StatusOr<std::unique_ptr<PreparedConfig>> Prepare(const Options& options);

} // namespace hm::tracker_reid
