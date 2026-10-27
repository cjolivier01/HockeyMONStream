#include "hstream/src/libs/common/UserConfig.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}

std::string contents(const fs::path& path) {
  std::ifstream input(path);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Identity {
  struct stat value{};
  std::string bytes;
  explicit Identity(const fs::path& path) : bytes(contents(path)) {
    require(::stat(path.c_str(), &value) == 0, "Cannot stat test preferences");
  }
  bool unchanged(const fs::path& path) const {
    struct stat current{};
    return ::stat(path.c_str(), &current) == 0 && bytes == contents(path) && value.st_dev == current.st_dev &&
        value.st_ino == current.st_ino && value.st_size == current.st_size &&
        value.st_mtim.tv_sec == current.st_mtim.tv_sec && value.st_mtim.tv_nsec == current.st_mtim.tv_nsec &&
        value.st_ctim.tv_sec == current.st_ctim.tv_sec && value.st_ctim.tv_nsec == current.st_ctim.tv_nsec;
  }
};

void fresh_home(const fs::path& home) {
  fs::create_directories(home);
  ::setenv("HOME", home.c_str(), 1);
  ::setenv("HSTREAM_JOB_MANAGED_RUNTIME", "1", 1);
  const fs::path legacy = *hm::user_config::file_path();
  const fs::path defaults = *hm::user_config::defaults_path();
  const auto preferences = hm::user_config::load_preferences_or_create();
  const auto snapshot = hm::user_config::load_snapshot_or_create();
  const auto overlay = hm::user_config::load_or_create();
  require(
      preferences.ok() && preferences->IsMap() && preferences->size() == 0,
      "Managed fresh preferences must be an empty overlay");
  require(
      snapshot.ok() && snapshot->values.IsMap() && snapshot->values.size() == 0 && snapshot->path == legacy,
      "Managed snapshot must retain legacy reference provenance without creating it");
  require(overlay.ok() && overlay->IsMap() && overlay->size() == 0, "Managed load must not invent tuning defaults");
  require(
      !fs::exists(legacy.parent_path()) && !fs::exists(defaults.parent_path()),
      "Managed readers created first-run settings directories");
  const auto managed_game = hm::user_config::game_root(*overlay);
  const auto managed_output = hm::user_config::output_root(*overlay);
  require(managed_game.ok() && managed_output.ok(), "Managed conventional paths failed");

  ::unsetenv("HSTREAM_JOB_MANAGED_RUNTIME");
  const auto ordinary = hm::user_config::load_snapshot_or_create();
  require(
      ordinary.ok() && fs::is_regular_file(legacy) && !fs::exists(defaults),
      "Ordinary first-run preference creation changed");
  require(
      ordinary->values.size() == 1 && ordinary->values["paths"].size() == 1 &&
          ordinary->values["paths"]["output-root"].IsScalar(),
      "Ordinary first-run document must contain only its existing path preference");
  const auto ordinary_game = hm::user_config::game_root(ordinary->values);
  const auto ordinary_output = hm::user_config::output_root(ordinary->values);
  require(
      ordinary_game.ok() && ordinary_output.ok() && *managed_game == *ordinary_game &&
          *managed_output == *ordinary_output,
      "Managed and ordinary first-run effective paths differ");
  const Identity original(legacy);
  ::setenv("HSTREAM_JOB_MANAGED_RUNTIME", "1", 1);
  require(
      hm::user_config::load_or_create().ok() && original.unchanged(legacy),
      "Managed reader changed an ordinary first-run file");
}

void existing_home(const fs::path& home) {
  fs::create_directories(home / ".hstream");
  ::setenv("HOME", home.c_str(), 1);
  ::setenv("HSTREAM_JOB_MANAGED_RUNTIME", "1", 1);
  const fs::path legacy = *hm::user_config::file_path();
  const fs::path defaults = *hm::user_config::defaults_path();
  std::ofstream(legacy) << "# User-owned preferences\npaths: {game-root: '~/media', output-root: work}\n"
                           "runtime: {gpu_memory_profile: low}\nstitching: {control_point_limit: 77}\n";
  const Identity original(legacy);
  const auto inherited = hm::user_config::load_snapshot_or_create();
  require(
      inherited.ok() && inherited->path == legacy &&
          inherited->values["runtime"]["gpu_memory_profile"].as<std::string>() == "low",
      "Managed load stopped reading existing legacy tuning");
  fs::create_directories(defaults.parent_path());
  std::ofstream(defaults) << "# User-owned Defaults\nruntime: {gpu_memory_profile: standard}\n";
  const Identity selected(defaults);
  const auto preferences = hm::user_config::load_preferences_or_create();
  const auto active = hm::user_config::load_snapshot_or_create();
  const auto overlay = hm::user_config::load_or_create();
  require(
      preferences.ok() && (*preferences)["runtime"]["gpu_memory_profile"].as<std::string>() == "low",
      "Managed preferences reader must still return the legacy document");
  require(
      active.ok() && active->path == defaults &&
          active->values["runtime"]["gpu_memory_profile"].as<std::string>() == "standard" &&
          !active->values["stitching"] && overlay.ok(),
      "Managed Defaults must replace legacy tuning while retaining active provenance");
  const auto game = hm::user_config::game_root(*overlay);
  const auto output = hm::user_config::output_root(*overlay);
  require(
      game.ok() && *game == home / "media" && output.ok() && *output == home / "work",
      "Managed Defaults dropped legacy paths");
  require(
      original.unchanged(legacy) && selected.unchanged(defaults),
      "Managed readers changed preference bytes or file identities");

  std::ofstream(defaults) << "{}\n";
  const Identity reset(defaults);
  const auto cleared = hm::user_config::load_or_create();
  require(
      cleared.ok() && !(*cleared)["runtime"] && !(*cleared)["stitching"] && (*cleared)["paths"],
      "Empty Defaults resurrected legacy tuning");
  require(original.unchanged(legacy) && reset.unchanged(defaults), "Managed Reset read changed a file");
  std::ofstream(defaults) << "runtime: [\n";
  const Identity malformed(defaults);
  require(
      !hm::user_config::load_or_create().ok() && malformed.unchanged(defaults) && original.unchanged(legacy),
      "Managed malformed Defaults must fail without writes");
}

void defaults_without_legacy(const fs::path& home) {
  fs::create_directories(home / ".config/hstream-ui");
  ::setenv("HOME", home.c_str(), 1);
  ::setenv("HSTREAM_JOB_MANAGED_RUNTIME", "1", 1);
  const fs::path legacy = *hm::user_config::file_path();
  const fs::path defaults = *hm::user_config::defaults_path();
  std::ofstream(defaults) << "runtime: {gpu_memory_profile: low}\n";
  const Identity original(defaults);
  const auto active = hm::user_config::load_snapshot_or_create();
  require(
      active.ok() && active->path == defaults &&
          active->values["runtime"]["gpu_memory_profile"].as<std::string>() == "low" &&
          !fs::exists(legacy.parent_path()) && original.unchanged(defaults),
      "Managed Defaults-only HOME created legacy preferences or changed Defaults");
}
} // namespace

int main() {
  const std::string pattern = (fs::temp_directory_path() / "hstream-user-managed-XXXXXX").string();
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  const char* created = ::mkdtemp(temporary.data());
  if (!created) {
    std::cerr << "Cannot create owned temporary HOME root\n";
    return 1;
  }
  const fs::path root(created);
  ::unsetenv("HM_GAME_DIR");
  ::unsetenv("HM_OUTPUT_WORK_DIR");
  ::unsetenv("XDG_CONFIG_HOME");
  bool ok = true;
  try {
    fresh_home(root / "fresh");
    existing_home(root / "existing");
    defaults_without_legacy(root / "defaults-only");
  } catch (const std::exception& error) {
    std::cerr << error.what() << "\n";
    ok = false;
  }
  fs::remove_all(root);
  std::cout << (ok ? "PASS" : "FAIL")
            << ": managed preferences are read-only with unchanged effective paths and precedence\n";
  return ok ? 0 : 1;
}
