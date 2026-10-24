#include "hstream/src/libs/program/ProgramFileOutput.h"

#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
void Check(bool ok, const std::string& message) {
  if (!ok)
    throw std::runtime_error(message);
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
    fs::remove(root);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
