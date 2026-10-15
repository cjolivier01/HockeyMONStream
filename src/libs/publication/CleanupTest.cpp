#include "hstream/src/libs/publication/Cleanup.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace pub = hm::publication;
namespace {
void require(bool result, const std::string& message) {
  if (!result)
    throw std::runtime_error(message);
}
YAML::Node identity(const fs::path& path) {
  struct stat st{};
  require(::lstat(path.c_str(), &st) == 0, "inspect fixture");
  YAML::Node node;
  node["device"] = uint64_t(st.st_dev);
  node["inode"] = uint64_t(st.st_ino);
  node["size"] = int64_t(st.st_size);
  node["mtime"] = int64_t(st.st_mtim.tv_sec);
  node["mtime_ns"] = int64_t(st.st_mtim.tv_nsec);
  node["directory"] = bool(S_ISDIR(st.st_mode));
  return node;
}
std::string read(const fs::path& path) {
  std::ifstream file(path);
  return std::string(std::istreambuf_iterator<char>(file), {});
}
} // namespace
int main() {
  try {
    const fs::path base = fs::path(std::getenv("TEST_TMPDIR") ? std::getenv("TEST_TMPDIR") : "/tmp") /
        ("publication-cleanup-test-" + std::to_string(::getpid()));
    fs::create_directories(base);
    const auto journal = base / "receipt.yaml";
    YAML::Node run;
    const auto checkpoint = [&] {
      std::ofstream file(journal);
      file << run;
      require(bool(file), "save recovery fixture");
    };
    for (const char* point : {"after-directory-create", "after-quarantine", "after-delete", "after-directory-remove"}) {
      for (const bool directory : {false, true}) {
        const auto parent = base / (std::string(point) + (directory ? "-directory" : "-file"));
        fs::create_directories(parent);
        const auto source = parent / "owned";
        if (directory)
          fs::create_directory(source);
        else
          std::ofstream(source) << "owned contents";
        const auto expected = identity(source);
        setenv("HSTREAM_TEST_CLEANUP_STOP", point, 1);
        bool interrupted = false;
        try {
          pub::CleanupOwned(source, expected, !directory, run, checkpoint);
        } catch (const std::exception&) {
          interrupted = true;
        }
        unsetenv("HSTREAM_TEST_CLEANUP_STOP");
        require(interrupted, "injected cleanup interruption");
        run = YAML::LoadFile(journal.string());
        pub::ResumeCleanup(run, checkpoint);
        require(!run["cleanup_pending"], "pending operation retired after recovery");
        require(fs::is_empty(parent), "recovery removes source and quarantine");
        pub::CleanupOwned(source, expected, !directory, run, checkpoint);
      }
    }
    // A new file at the old public name after quarantine must never be unlinked.
    const auto replaced_parent = base / "replacement";
    fs::create_directory(replaced_parent);
    const auto source = replaced_parent / "source";
    std::ofstream(source) << "original";
    const auto expected = identity(source);
    setenv("HSTREAM_TEST_CLEANUP_STOP", "after-quarantine", 1);
    try {
      pub::CleanupOwned(source, expected, true, run, checkpoint);
    } catch (const std::exception&) {
    }
    unsetenv("HSTREAM_TEST_CLEANUP_STOP");
    std::ofstream(source) << "foreign replacement";
    run = YAML::LoadFile(journal.string());
    pub::ResumeCleanup(run, checkpoint);
    require(read(source) == "foreign replacement", "public replacement survives cleanup recovery");
    require(
        std::distance(fs::directory_iterator(replaced_parent), fs::directory_iterator{}) == 1, "no quarantine remains");

    // Simulate the atomic rename capturing a replacement after the original
    // identity check. Recovery restores it, including the NFS link fallback.
    for (const bool fallback : {false, true}) {
      const auto parent = base / (fallback ? "captured-fallback" : "captured");
      fs::create_directory(parent);
      const auto path = parent / "source";
      std::ofstream(path) << "original";
      const auto original = identity(path);
      setenv("HSTREAM_TEST_CLEANUP_STOP", "after-quarantine", 1);
      try {
        pub::CleanupOwned(path, original, true, run, checkpoint);
      } catch (const std::exception&) {
      }
      unsetenv("HSTREAM_TEST_CLEANUP_STOP");
      run = YAML::LoadFile(journal.string());
      const auto captured = fs::path(run["cleanup_pending"]["quarantine"].as<std::string>()) / "entry";
      fs::rename(captured, base / (fallback ? "saved-fallback" : "saved-original"));
      std::ofstream(captured) << "foreign captured file";
      if (fallback)
        setenv("HSTREAM_TEST_CLEANUP_NO_RENAME_NOREPLACE", "1", 1);
      if (fallback) {
        setenv("HSTREAM_TEST_CLEANUP_STOP", "after-restore-link", 1);
        try {
          pub::ResumeCleanup(run, checkpoint);
        } catch (const std::exception&) {
        }
        unsetenv("HSTREAM_TEST_CLEANUP_STOP");
        require(
            fs::exists(captured) && read(path) == "foreign captured file", "interrupted fallback preserves both links");
        run = YAML::LoadFile(journal.string());
      }
      bool refused = false;
      try {
        pub::ResumeCleanup(run, checkpoint);
      } catch (const std::exception&) {
        refused = true;
      }
      unsetenv("HSTREAM_TEST_CLEANUP_NO_RENAME_NOREPLACE");
      require(refused && read(path) == "foreign captured file", "captured replacement restored without deletion");
      require(!run["cleanup_pending"], "restored replacement leaves no pending cleanup");
      require(
          std::distance(fs::directory_iterator(parent), fs::directory_iterator{}) == 1,
          "restoration retires quarantine");
    }
    // A foreign public entry blocks restoration; both foreign files survive.
    const auto blocked = base / "blocked";
    fs::create_directory(blocked);
    const auto blocked_source = blocked / "source";
    std::ofstream(blocked_source) << "original";
    const auto blocked_identity = identity(blocked_source);
    setenv("HSTREAM_TEST_CLEANUP_STOP", "after-quarantine", 1);
    try {
      pub::CleanupOwned(blocked_source, blocked_identity, true, run, checkpoint);
    } catch (const std::exception&) {
    }
    unsetenv("HSTREAM_TEST_CLEANUP_STOP");
    run = YAML::LoadFile(journal.string());
    const auto retained = fs::path(run["cleanup_pending"]["quarantine"].as<std::string>()) / "entry";
    fs::rename(retained, base / "saved-blocked");
    std::ofstream(retained) << "captured replacement";
    std::ofstream(blocked_source) << "new public replacement";
    bool refused = false;
    try {
      pub::ResumeCleanup(run, checkpoint);
    } catch (const std::exception&) {
      refused = true;
    }
    require(
        refused && read(retained) == "captured replacement" && read(blocked_source) == "new public replacement",
        "restoration cannot overwrite a newer public entry");
    fs::remove(blocked_source);
    try {
      pub::ResumeCleanup(run, checkpoint);
    } catch (const std::exception&) {
    }
    require(read(blocked_source) == "captured replacement" && !run["cleanup_pending"], "restoration can be retried");

    // Do not move a directory already known to contain unowned entries.
    const auto nonempty = base / "nonempty";
    fs::create_directory(nonempty);
    const auto directory_identity = identity(nonempty);
    std::ofstream(nonempty / "foreign") << "retained";
    refused = false;
    try {
      pub::CleanupOwned(nonempty, directory_identity, false, run, checkpoint);
    } catch (const std::exception&) {
      refused = true;
    }
    require(
        refused && fs::exists(nonempty / "foreign") && !run["cleanup_pending"], "nonempty directory remains in place");
    const auto real_parent = base / "actual-parent";
    fs::create_directory(real_parent);
    const auto alias_parent = base / "parent-alias";
    fs::create_directory_symlink(real_parent, alias_parent);
    std::ofstream(alias_parent / "owned") << "owned";
    pub::CleanupOwned(alias_parent / "owned", identity(alias_parent / "owned"), true, run, checkpoint);
    require(fs::is_empty(real_parent), "explicit symlinked archive directories work");
    fs::remove_all(base);
    std::cout << "Cleanup tests passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
