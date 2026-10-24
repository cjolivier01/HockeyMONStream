#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include <migraphx/migraphx.hpp>
#include "hstream/src/libs/gpu/migraphx/MigraphxCacheConfig.h"

namespace hm::gpu {
// Startup-only private cache transaction. The parsed program includes external
// ONNX tensor data, so neither model paths nor timestamps stand in for weights.
// The GPU target must be loaded before Prepare fingerprints its runtime.
class MigraphxCache {
 public:
  static std::unique_ptr<MigraphxCache> Prepare(
      const MigraphxCacheConfig& config,
      const migraphx::program& parsed,
      int device,
      bool fp16);
  ~MigraphxCache();
  bool Load(migraphx::program& program);
  void Publish(const migraphx::program& program);
  const std::string& key() const {
    return key_;
  }

 private:
  explicit MigraphxCache(MigraphxCacheConfig config);
  MigraphxCacheConfig config_;
  std::filesystem::path scratch_;
  std::string key_;
};
} // namespace hm::gpu
