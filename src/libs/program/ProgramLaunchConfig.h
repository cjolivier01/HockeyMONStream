#pragma once

#include <array>
#include <filesystem>
#include <vector>

#include "hstream/src/libs/common/ConfigLayers.h"
#include "hstream/src/libs/program/AmdProgramSettings.h"

namespace hm::program {
struct ProgramLaunchConfig {
  std::filesystem::path game, application_file, inference_file;
  hm::config::ProgramConfiguration configuration;
  YAML::Node camera, inference;
  AmdProgramSettings amd;
  std::array<std::vector<std::filesystem::path>, 2> chapters;
  std::array<double, 2> frame_offsets;
};

// Resolve saved Program playback configuration before GPU/model preparation.
// Game root follows the existing baseline/user/environment contract. Native
// app YAML is explicit; user/game/launch overrides preserve the CLI's precedence.
// Camera configuration is returned in memory; no runtime YAML is published.
// Original camera lists and synchronization must already be saved by calibration.
absl::StatusOr<ProgramLaunchConfig> LoadProgramLaunchConfig(
    const std::string& game_id,
    const std::filesystem::path& application_file,
    const std::vector<std::string>& option_groups);
} // namespace hm::program
