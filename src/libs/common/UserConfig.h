#pragma once

#include <filesystem>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::user_config {

inline constexpr char kPathsKey[] = "paths";
inline constexpr char kGameRootKey[] = "game-root";
inline constexpr char kOutputRootKey[] = "output-root";

// The per-user overlay is loaded between the baseline and game-private YAML.
// Its first-run document intentionally contains only paths.output-root; the
// game root continues to have a conventional default until the user changes it.
// If HOME is unavailable but HM_GAME_DIR is explicit, loading yields an empty
// overlay; HM_OUTPUT_WORK_DIR remains explicit whenever archive output is used.
absl::StatusOr<std::filesystem::path> file_path();
absl::StatusOr<YAML::Node> load_or_create();

absl::StatusOr<std::filesystem::path> game_root(const YAML::Node& config);
absl::StatusOr<std::filesystem::path> output_root(const YAML::Node& config);

} // namespace hm::user_config
