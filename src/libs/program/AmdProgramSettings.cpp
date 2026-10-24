#include "hstream/src/libs/program/AmdProgramSettings.h"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::program {
namespace {
YAML::Node Required(const YAML::Node& config, const std::string& path) {
  const auto value = get_node(config, "runtime.amd." + path);
  if (!value || !value->IsScalar())
    throw std::invalid_argument("Missing scalar runtime.amd." + path);
  return *value;
}
template <typename T>
T Integer(const YAML::Node& config, const std::string& path, bool allow_zero = false) {
  const auto value = Required(config, path).as<int64_t>();
  if (value < (allow_zero ? 0 : 1) || static_cast<uint64_t>(value) > std::numeric_limits<T>::max())
    throw std::invalid_argument("Out-of-range runtime.amd." + path);
  return static_cast<T>(value);
}
uint64_t Nanoseconds(const YAML::Node& config, const std::string& path) {
  const auto ms = Integer<uint64_t>(config, path);
  if (ms > uint64_t(std::numeric_limits<int64_t>::max()) / 1000000)
    throw std::invalid_argument("Timeout overflows nanoseconds: runtime.amd." + path);
  return ms * 1000000;
}
} // namespace

absl::StatusOr<AmdProgramSettings> ResolveAmdProgramSettings(const YAML::Node& config) {
  try {
    AmdProgramSettings result;
    result.gpu_id = Integer<int>(config, "gpu_id", true);
    result.reader_queue_frames = Integer<unsigned>(config, "reader_queue_frames");
    result.output_pool_frames = Integer<unsigned>(config, "output_pool_frames");
    result.operation_timeout_ns = Nanoseconds(config, "operation_timeout_ms");
    result.cancellation_poll_ns = Nanoseconds(config, "cancellation_poll_ms");
    if (result.cancellation_poll_ns > result.operation_timeout_ns)
      return absl::InvalidArgumentError("runtime.amd.cancellation_poll_ms exceeds operation_timeout_ms");
    result.maximum_inference_io_bytes = Integer<size_t>(config, "maximum_inference_io_bytes");
    result.maximum_encoded_packet_bytes = Integer<int>(config, "maximum_encoded_packet_bytes");
    result.maximum_runtime_command_bytes = Integer<size_t>(config, "maximum_runtime_command_bytes");
    result.compact_stitch_workspace = Required(config, "compact_stitch_workspace").as<bool>();
    const bool cache_enabled = Required(config, "model_cache.enable").as<bool>();
    auto cache_directory = Required(config, "model_cache.directory").as<std::string>();
    const auto cache_bytes = Integer<size_t>(config, "model_cache.maximum_entry_bytes");
    if (cache_enabled) {
      const std::string token = "{user_cache}";
      if (cache_directory.rfind(token + '/', 0) == 0) {
        const char* xdg = std::getenv("XDG_CACHE_HOME");
        const char* home = std::getenv("HOME");
        std::filesystem::path root;
        if (xdg && *xdg) {
          root = xdg;
        } else if (home && *home) {
          root = std::filesystem::path(home) / ".cache";
        } else {
          return absl::InvalidArgumentError("model_cache requires HOME, XDG_CACHE_HOME or an absolute directory");
        }
        cache_directory.replace(0, token.size(), root.string());
      }
      if (!std::filesystem::path(cache_directory).is_absolute())
        return absl::InvalidArgumentError("runtime.amd.model_cache.directory must resolve to an absolute path");
      result.model_cache = gpu::MigraphxCacheConfig{cache_directory, cache_bytes};
    }
    result.tracker = {
        Required(config, "tracker.low_score").as<float>(),
        Required(config, "tracker.high_score").as<float>(),
        Required(config, "tracker.new_track_score").as<float>(),
        Required(config, "tracker.high_match_cost").as<float>(),
        Required(config, "tracker.low_match_cost").as<float>(),
        Required(config, "tracker.tentative_match_cost").as<float>(),
        Required(config, "tracker.weight_high_matches_by_score").as<bool>(),
        Integer<uint64_t>(config, "tracker.keep_lost_frames", true),
        Integer<size_t>(config, "tracker.confirmation_frames"),
        Integer<size_t>(config, "tracker.maximum_tracks"),
        Integer<size_t>(config, "tracker.maximum_cost_matrix_bytes")};
    const auto valid = tracking::ValidateTrackerConfig(result.tracker);
    if (!valid.ok())
      return valid;
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid AMD Program settings: ") + error.what());
  }
}
} // namespace hm::program
