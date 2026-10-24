#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include "hstream/src/libs/gpu/migraphx/MigraphxCacheConfig.h"
#include "hstream/src/libs/tracking/NativeTracker.h"
#include "yaml-cpp/yaml.h"

namespace hm::program {
// Application-owned limits and tuning, resolved from the shared baseline and
// overlays before any GPU/decoder preparation. These are not GPU-library defaults.
struct AmdProgramSettings {
  int gpu_id;
  unsigned reader_queue_frames, output_pool_frames;
  uint64_t operation_timeout_ns, cancellation_poll_ns;
  size_t maximum_inference_io_bytes, maximum_encoded_packet_bytes;
  size_t maximum_runtime_command_bytes;
  bool compact_stitch_workspace;
  std::optional<gpu::MigraphxCacheConfig> model_cache;
  tracking::TrackerConfig tracker;
};

absl::StatusOr<AmdProgramSettings> ResolveAmdProgramSettings(const YAML::Node& effective_config);
} // namespace hm::program
