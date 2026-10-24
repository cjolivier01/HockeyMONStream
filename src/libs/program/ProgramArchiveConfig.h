#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>

#include "hstream/src/libs/common/VideoBitrate.h"
#include "hstream/src/libs/program/ProgramOutputConfig.h"

namespace hm::program {
struct ProgramArchiveConfig {
  int sink_id;
  std::string kind;
  std::filesystem::path path;
  ProgramOutputConfig output;
};
// Pure preflight: no directories, files or GPU resources are created. Native
// sink settings retain same-layer precedence. A managed run supplies its own
// private directory; ordinary launches supply the configured output root/game.
absl::StatusOr<std::map<std::string, ProgramArchiveConfig>> ResolveProgramArchives(
    const config::ProgramConfiguration& config,
    const std::set<std::string>& sinks,
    const ProgramOutputConfig& program,
    const std::filesystem::path& directory,
    const std::optional<BitratePerPixel>& source_bitrate);
} // namespace hm::program
