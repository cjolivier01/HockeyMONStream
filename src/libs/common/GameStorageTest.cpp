#include "hstream/src/libs/common/GameStorage.h"

#include <sys/wait.h>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

#include "yaml-cpp/yaml.h"

namespace fs = std::filesystem;
using namespace hm::game_storage;

namespace {
void Check(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
YAML::Node Inventory(const fs::path& game) {
  return YAML::LoadFile(StatePath(game, "manifest.json").string());
}
YAML::Node Find(const YAML::Node& inventory, const std::string& path) {
  for (const auto& entry : inventory["files"])
    if (entry["path"].as<std::string>() == path)
      return entry;
  return YAML::Node(YAML::NodeType::Undefined);
}
void Run(const fs::path& root) {
  const auto game = root / "game";
  fs::create_directory(game);
  std::ofstream(game / "source.mp4") << "source";
  std::ofstream(game / "config.yaml") << "legacy file must remain untouched";
  Check(!fs::exists(StateDirectory(game)), "path reads must not create state");
  Check(EnsureStateDirectory(game).ok(), "create state");
  const auto external_work = ConfineWorkDirectory(game, root / "external-work");
  const auto internal_work = ConfineWorkDirectory(game, game / "runs/run-1");
  const auto root_work = ConfineWorkDirectory(game, game);
  Check(external_work.ok() && *external_work == root / "external-work", "external work stays external");
  Check(internal_work.ok() && *internal_work == StatePath(game, "work/runs/run-1"), "in-game work is private");
  Check(root_work.ok() && *root_work == StatePath(game, "work"), "game-root work is private");
  Check(ConfineWorkDirectory(game, *internal_work).value() == *internal_work, "work confinement is idempotent");
  std::ofstream(StatePath(game, "config.yaml")) << "saved config";
  fs::create_directories(StatePath(game, "calibration-matches/inputs"));
  std::ofstream(StatePath(game, "calibration-matches/inputs/frame.png")) << "frame";
  fs::create_directory(root / "outside");
  std::ofstream(root / "outside/secret") << "not game state";
  fs::create_directory_symlink(root / "outside", StatePath(game, "alias"));
  Check(RefreshManifest(game, "calibration").ok(), "refresh inventory");
  auto inventory = Inventory(game);
  Check(inventory["schema_version"].as<int>() == 1, "version");
  Check(Find(inventory, "_hockeymon/config.yaml")["role"].as<std::string>() == "saved-state", "saved config");
  Check(
      Find(inventory, "_hockeymon/calibration-matches/inputs/frame.png")["size_bytes"].as<int>() == 5,
      "nested file size");
  Check(Find(inventory, "_hockeymon/alias")["kind"].as<std::string>() == "symlink", "symlink inventoried");
  Check(!Find(inventory, "_hockeymon/alias/secret"), "must not follow symlinks");
  Check(!Find(inventory, "source.mp4") && !Find(inventory, "config.yaml"), "must not adopt root files");
  fs::create_directory(game / "cam1");
  fs::create_symlink(root / "outside/secret", game / "cam1/imported.mp4");
  Check(
      RegisterInput(game, "cam1", "source-import").ok() &&
          RegisterInput(game, "cam1/imported.mp4", "source-import").ok(),
      "register imported source directory and alias");
  Check(RefreshManifest(game, "calibration").ok(), "retain input references");
  inventory = Inventory(game);
  Check(
      Find(inventory, "cam1/imported.mp4")["role"].as<std::string>() == "input-reference" &&
          Find(inventory, "cam1/imported.mp4")["kind"].as<std::string>() == "symlink",
      "import reference never adopts its target");
  fs::create_directories(StatePath(game, "hstream-ui/left"));
  fs::create_symlink(root / "outside/secret", StatePath(game, "hstream-ui/left/imported.mp4"));
  Check(RegisterInput(game, "_hockeymon/hstream-ui/left", "source-import").ok(), "register private input topology");
  Check(
      RegisterInput(game, "_hockeymon/hstream-ui/left/imported.mp4", "source-import").ok(),
      "register private input alias");
  Check(RefreshManifest(game, "calibration").ok(), "preserve private input registration");
  inventory = Inventory(game);
  Check(
      Find(inventory, "_hockeymon/hstream-ui/left")["role"].as<std::string>() == "input-reference" &&
          Find(inventory, "_hockeymon/hstream-ui/left/imported.mp4")["role"].as<std::string>() == "input-reference" &&
          Find(inventory, "_hockeymon/hstream-ui/left/imported.mp4")["kind"].as<std::string>() == "symlink",
      "private imported references keep their role across refresh");
  Check(!RegisterPublished(game, "_hockeymon/config.yaml", "bad").ok(), "private state cannot be a public output");
  Check(!Find(inventory, "_hockeymon/manifest.json"), "manifest cannot inventory its own changing size");
  const std::string unusual = "quote\"-control\x01-name";
  std::ofstream(StatePath(game, unusual)) << "valid filename";
  std::ofstream(StatePath(game, ".manifest.tmp-abandoned")) << "interrupted inventory update";
  fs::create_directories(StatePath(game, "hstream-stitch-scratch/nested"));
  std::ofstream(StatePath(game, "hstream-stitch-scratch/nested/frame.png")) << "scratch";
  Check(RefreshManifest(game, "calibration").ok(), "inventory unusual filenames");
  inventory = Inventory(game);
  Check(bool(Find(inventory, "_hockeymon/" + unusual)), "filename roundtrip");
  Check(
      Find(inventory, "_hockeymon/.manifest.tmp-abandoned")["role"].as<std::string>() == "temporary",
      "abandoned inventory scratch must remain visible");
  std::ifstream encoded(StatePath(game, "manifest.json"));
  const std::string json((std::istreambuf_iterator<char>(encoded)), std::istreambuf_iterator<char>());
  Check(
      json.find("\\u0001") != std::string::npos && json.find("\\x01") == std::string::npos,
      "manifest must use JSON escapes, not YAML-only escapes");
  Check(
      Find(inventory, "_hockeymon/hstream-stitch-scratch/nested/frame.png")["role"].as<std::string>() == "temporary",
      "scratch children inherit directory role");
  std::ofstream(game / "published.mp4") << "output";
  Check(RegisterPublished(game, game / "published.mp4", "publication", "run-1").ok(), "register output");
  Check(RefreshManifest(game, "stitching").ok(), "preserve publication registration");
  inventory = Inventory(game);
  Check(Find(inventory, "published.mp4")["run_id"].as<std::string>() == "run-1", "retain publication producer");
  Check(Find(inventory, "_hockeymon/config.yaml")["producer"].as<std::string>() == "calibration", "retain provenance");
  fs::remove(StatePath(game, "calibration-matches/inputs/frame.png"));
  Check(RefreshManifest(game, "cleanup").ok(), "refresh after cleanup");
  Check(!Find(Inventory(game), "_hockeymon/calibration-matches/inputs/frame.png"), "retire deleted entries");
  Check(!RegisterPublished(game, "../outside/secret", "bad").ok(), "reject publication escape");
  fs::create_directory_symlink(root / "outside", game / "external");
  Check(!RegisterPublished(game, "external/secret", "bad").ok(), "reject linked publication parent");
  bool rejected = false;
  try {
    StatePath(game, "../secret");
  } catch (const std::invalid_argument&) {
    rejected = true;
  }
  Check(rejected, "reject escaped state paths");

  // Separate processes concurrently register outputs: lock+read+replace must
  // preserve all entries, not overwrite another operation's publication.
  pid_t children[4];
  for (int i = 0; i < 4; ++i) {
    const std::string name = "published-" + std::to_string(i) + ".mp4";
    std::ofstream(game / name) << "output";
    children[i] = ::fork();
    Check(children[i] >= 0, "fork");
    if (children[i] == 0)
      ::_exit(RegisterPublished(game, name, "publication", name).ok() ? 0 : 1);
  }
  for (const auto child : children) {
    int status{};
    Check(
        ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "concurrent register");
  }
  inventory = Inventory(game);
  for (int i = 0; i < 4; ++i)
    Check(bool(Find(inventory, "published-" + std::to_string(i) + ".mp4")), "lost concurrent publication");

  // Passing the private directory on as the game used to mkdir a second one
  // inside it and write that call's locks and journals where nothing reads them.
  const auto nested = StateDirectory(game);
  Check(
      EnsureStateDirectory(nested).code() == absl::StatusCode::kInvalidArgument,
      "a nested state root is a bad argument, not a missing one");
  Check(!EnsureStateDirectory(nested.string() + "/").ok(), "reject a trailing separator");
  Check(!RefreshManifest(nested, "test").ok(), "inventory must reject nested state root");
  Check(!ConfineWorkDirectory(nested, nested / "work").ok(), "work must reject nested state root");
  // An external request returns before the work root is ever created, so this
  // one only fails if ConfineWorkDirectory checks the game itself.
  Check(!ConfineWorkDirectory(nested, root / "external-work").ok(), "work must reject it for any request");
  Check(!fs::exists(StatePath(game, "_hockeymon")), "a rejected game must leave no nested state");
  // Experiment candidate games live below a private directory without being
  // one, and must stay usable.
  const auto candidate = StatePath(game, "stitching-experiments/sessions/session-a/demo-stitch-exp-1");
  fs::create_directories(candidate);
  Check(EnsureStateDirectory(candidate).ok(), "a game below private state is still a game");
  Check(EnsureStateDirectory(game).ok(), "the game root itself still resolves");

  const auto linked = root / "linked-game";
  fs::create_directory(linked);
  fs::create_directory_symlink(root / "outside", StateDirectory(linked));
  Check(!EnsureStateDirectory(linked).ok() && !RefreshManifest(linked, "test").ok(), "reject linked state root");
  Check(!ConfineWorkDirectory(linked, StatePath(linked, "work")).ok(), "work must reject linked state root");
  Check(!fs::exists(root / "outside/manifest.json"), "no outside writes");
  fs::remove(StatePath(game, "manifest.json"));
  fs::create_symlink(root / "outside/secret", StatePath(game, "manifest.json"));
  Check(!RefreshManifest(game, "test").ok(), "reject linked manifest");
}
} // namespace

int main() {
  char pattern[] = "/tmp/hstream-game-storage-XXXXXX";
  const char* created = ::mkdtemp(pattern);
  if (!created)
    return 1;
  const fs::path root(created);
  int result = 0;
  try {
    Run(root);
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    result = 1;
  }
  fs::remove_all(root);
  return result;
}
