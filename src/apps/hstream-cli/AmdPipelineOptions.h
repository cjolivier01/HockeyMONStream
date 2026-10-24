#pragma once

#include <cstdint>
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
};
// Includes argv[0]. Parse and reject unsupported modes before opening a game,
// loading GPU libraries or modifying output files. --help uses GLib's help.
absl::StatusOr<AmdPipelineOptions> ParseAmdPipelineOptions(std::vector<std::string> arguments);
} // namespace hm::program
