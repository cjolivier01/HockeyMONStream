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
// The mobile copy's keyframe interval is configured in seconds and set on the
// encoder in frames, so that route additionally requires the exact source frame
// rate as numerator/denominator. Omitting it is an error only when that route
// is selected.
absl::StatusOr<std::map<std::string, ProgramArchiveConfig>> ResolveProgramArchives(
    const config::ProgramConfiguration& config,
    const std::set<std::string>& sinks,
    const ProgramOutputConfig& program,
    const std::filesystem::path& directory,
    const std::optional<BitratePerPixel>& source_bitrate,
    const std::optional<std::pair<int, int>>& source_frame_rate = std::nullopt);
} // namespace hm::program
