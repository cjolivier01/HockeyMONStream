#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << "FAIL: expected hmstream-cli and calibration config paths\n";
    return 1;
  }
  std::string pattern = (fs::temp_directory_path() / "hmstream-clean-only-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "FAIL: unable to create clean-only test directory\n";
    return 1;
  }
  const fs::path root(pattern);
  const fs::path game = root / "games" / "clean-only-test";
  fs::create_directories(game);
  std::ofstream(game / "seam_file.png") << "generated artifact\n";

  const pid_t child = ::fork();
  if (child == 0) {
    ::setenv("HOME", (root / "home").c_str(), 1);
    ::setenv("HM_GAME_DIR", (root / "games").c_str(), 1);
    ::setenv("HTTP_PROXY", "http://127.0.0.1:1", 1);
    ::setenv("HTTPS_PROXY", "http://127.0.0.1:1", 1);
    ::execl(argv[1], argv[1], "-g", "clean-only-test", "-c", argv[2], "--clean", static_cast<char*>(nullptr));
    _exit(127);
  }
  int status = 0;
  const bool child_ok =
      child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  const bool artifact_removed = !fs::exists(game / "seam_file.png");
  const bool no_asset_download = !fs::exists(root / "home" / ".cache" / "hmstream" / "models");
  fs::remove_all(root);
  if (!child_ok || !artifact_removed || !no_asset_download) {
    std::cerr << "FAIL: clean-only must remove artifacts without verifying or downloading pretrained models\n";
    return 1;
  }
  return 0;
}
