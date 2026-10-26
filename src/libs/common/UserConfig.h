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
// Legacy tuning is used only until defaults.yaml exists. Legacy paths remain
// supported after opting into the Defaults editor.
absl::StatusOr<YAML::Node> load_preferences_or_create();
absl::StatusOr<std::filesystem::path> defaults_path();
// Missing defaults are an empty overlay; malformed/unreadable files fail closed.
absl::StatusOr<YAML::Node> load_defaults();
// Resolves the active tuning document for relative native references.
absl::StatusOr<std::filesystem::path> active_file_path();
struct Snapshot {
  YAML::Node values;
  std::filesystem::path path;
};
// Keeps native-reference provenance attached to the exact loaded values.
absl::StatusOr<Snapshot> load_snapshot_or_create();
// Same active tuning precedence, without creating a first-run preferences file.
absl::StatusOr<Snapshot> load_snapshot();
// Loads the active tuning layer plus legacy paths, for both native backends.
absl::StatusOr<YAML::Node> load_or_create();

absl::StatusOr<std::filesystem::path> game_root(const YAML::Node& config);
absl::StatusOr<std::filesystem::path> output_root(const YAML::Node& config);

} // namespace hm::user_config
