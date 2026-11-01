#include "hstream/src/libs/program/ProgramFileOutput.h"

#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

#include "yaml-cpp/yaml.h"

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
}
YAML::Node InventoryEntry(const std::filesystem::path& game, const std::string& path) {
  const auto inventory = YAML::LoadFile((game / "_hockeymon/manifest.json").string());
  for (const auto& entry : inventory["files"])
    if (entry["path"].as<std::string>() == path)
      return entry;
  return YAML::Node(YAML::NodeType::Undefined);
}
void GameInventory(const std::filesystem::path& root) {
  namespace fs = std::filesystem;
  const auto game = root / "game";
  fs::create_directory(game);
  std::ofstream(game / "source.mp4") << "recording";
  const auto path = game / "program.mp4";
  {
    auto output = hm::program::ProgramFileOutput::Create(path);
    Check(output.ok(), output.status().ToString());
    Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
    Check(absl::IsCancelled((*output)->Commit([] { return true; }, game)), "Cancelled game output committed");
    Check(!fs::exists(path) && !fs::exists(game / "_hockeymon"), "Cancelled output created game inventory");
  }
  {
    auto output = hm::program::ProgramFileOutput::Create(path);
    Check(output.ok(), output.status().ToString());
    Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
    Check(!fs::exists(path), "Unfinished game output became visible");
    const auto status = (*output)->Commit({}, game);
    Check(status.ok(), status.ToString());
    const auto entry = InventoryEntry(game, "program.mp4");
    Check(
        entry && entry["role"].as<std::string>() == "published" &&
            entry["producer"].as<std::string>() == "amd-program" && entry["size_bytes"].as<int>() == 5,
        "Completed standalone game output is not inventoried");
    Check(!InventoryEntry(game, "source.mp4"), "Publication adopted unrelated source files");
  }
  {
    const auto private_path = game / "_hockeymon/work/private.mp4";
    fs::create_directories(private_path.parent_path());
    auto output = hm::program::ProgramFileOutput::Create(private_path);
    Check(output.ok(), output.status().ToString());
    Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
    const auto status = (*output)->Commit({}, game);
    Check(status.ok(), status.ToString());
    Check(
        InventoryEntry(game, "_hockeymon/work/private.mp4")["role"].as<std::string>() == "temporary",
        "Private output was published as a root asset");
    Check(
        InventoryEntry(game, "program.mp4")["role"].as<std::string>() == "published",
        "Private refresh lost public output");
  }
  {
    const auto old_directory = game / "old";
    const auto new_directory = game / "new";
    fs::create_directory(old_directory);
    auto output = hm::program::ProgramFileOutput::Create(old_directory / "moved.mp4");
    Check(output.ok(), output.status().ToString());
    Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
    fs::rename(old_directory, new_directory);
    fs::create_directory(old_directory);
    std::ofstream(old_directory / "moved.mp4") << "unrelated";
    const auto status = (*output)->Commit({}, game);
    Check(status.ok(), status.ToString());
    Check(
        InventoryEntry(game, "new/moved.mp4")["size_bytes"].as<int>() == 5 && !InventoryEntry(game, "old/moved.mp4"),
        "Inventory lost the pinned output directory after rename");
  }
  {
    const auto untouched_game = root / "untouched-game";
    fs::create_directory(untouched_game);
    const auto external = root / "external.mp4";
    auto output = hm::program::ProgramFileOutput::Create(external);
    Check(output.ok(), output.status().ToString());
    Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
    const auto status = (*output)->Commit({}, untouched_game);
    Check(status.ok(), status.ToString());
    Check(fs::is_regular_file(external) && fs::is_empty(untouched_game), "External export created game state");
  }
}
} // namespace
int main() {
  namespace fs = std::filesystem;
  try {
    const auto root = fs::path(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") /
        ("program-output-" + std::to_string(getpid()));
    Check(fs::create_directory(root), "Cannot create test root");
    const auto path = root / "video.mp4";
    {
      auto output = hm::program::ProgramFileOutput::Create(path);
      Check(output.ok(), output.status().ToString());
      Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
      Check(!fs::exists(path), "Partial output became public");
    }
    Check(fs::is_empty(root), "Cancelled output left an entry");
    {
      auto output = hm::program::ProgramFileOutput::Create(path);
      Check(output.ok(), output.status().ToString());
      Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
      Check((*output)->Commit({}).ok(), "Publication failed");
      Check(!(*output)->Commit({}).ok(), "Repeated commit accepted");
    }
    std::string contents;
    std::ifstream(path) >> contents;
    Check(contents == "video", "Published output changed");
    Check(!hm::program::ProgramFileOutput::Create(path).ok(), "Existing output accepted");
    fs::remove(path);
    {
      auto output = hm::program::ProgramFileOutput::Create(path);
      Check(output.ok(), output.status().ToString());
      fs::create_symlink("absent", path);
      Check(absl::IsAlreadyExists((*output)->Commit({})), "Publication overwrote concurrent symlink");
      Check(fs::read_symlink(path) == "absent", "Symlink changed");
      Check(!hm::program::ProgramFileOutput::Create(path).ok(), "Dangling symlink accepted");
    }
    fs::remove(path);
    {
      auto output = hm::program::ProgramFileOutput::Create(path);
      Check(output.ok(), output.status().ToString());
      Check(write((*output)->fd(), "video", 5) == 5, "Write failed");
      int checks = 0;
      Check(
          absl::IsCancelled((*output)->Commit([&] { return ++checks >= 2; })),
          "Cancellation during flush published output");
      Check(!fs::exists(path), "Cancelled publication became visible");
    }
    const auto child = fork();
    Check(child >= 0, "fork failed");
    if (!child) {
      auto output = hm::program::ProgramFileOutput::Create(path);
      if (!output.ok())
        _exit(2);
      kill(getpid(), SIGKILL);
      _exit(3);
    }
    int status = 0;
    Check(waitpid(child, &status, 0) == child && WIFSIGNALED(status), "Child did not terminate");
    Check(fs::is_empty(root), "SIGKILL left partial output");
    GameInventory(root);
    fs::remove_all(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
