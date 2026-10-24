#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace hm::program {
struct AmdPipelineOptions {
  std::string game, application;
  std::vector<std::string> options;
  std::set<std::string> sinks;
  std::optional<uint64_t> start, limit;
  bool realtime;
  uintptr_t window;
  std::map<std::string, uintptr_t> preview_windows;
  std::string preview_active;
  std::optional<std::string> saved_stitch_time, expected_invalidation;
};
// Includes argv[0]. Parse and reject unsupported modes before opening a game,
// loading GPU libraries or modifying output files. --help uses GLib's help.
absl::StatusOr<AmdPipelineOptions> ParseAmdPipelineOptions(std::vector<std::string> arguments);
} // namespace hm::program
