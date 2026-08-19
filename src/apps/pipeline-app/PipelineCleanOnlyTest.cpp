#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "FAIL: expected hstream-cli, calibration config, and baseline config paths\n";
    return 1;
  }
  std::string pattern = (fs::temp_directory_path() / "hstream-clean-only-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "FAIL: unable to create clean-only test directory\n";
    return 1;
  }
  const fs::path root(pattern);
  auto run_clean = [&](const char* game_id,
                       const std::vector<const char*>& clean_flags,
                       const char* runtime_invalidation_id,
                       const fs::path* leading_config = nullptr) {
    const pid_t child = ::fork();
    if (child == 0) {
      ::setenv("HOME", (root / "home").c_str(), 1);
      ::setenv("HM_GAME_DIR", (root / "games").c_str(), 1);
      ::setenv("HM_CONFIG_ROOT", (root / "config-root").c_str(), 1);
      ::setenv("HTTP_PROXY", "http://127.0.0.1:1", 1);
      ::setenv("HTTPS_PROXY", "http://127.0.0.1:1", 1);
      if (runtime_invalidation_id != nullptr)
        ::setenv("HSTREAM_CALIBRATION_INVALIDATION_ID", runtime_invalidation_id, 1);
      else
        ::unsetenv("HSTREAM_CALIBRATION_INVALIDATION_ID");
      std::vector<char*> child_argv = {
          argv[1],
          const_cast<char*>("-g"),
          const_cast<char*>(game_id),
      };
      if (leading_config != nullptr) {
        child_argv.push_back(const_cast<char*>("-c"));
        child_argv.push_back(const_cast<char*>(leading_config->c_str()));
      }
      child_argv.push_back(const_cast<char*>("-c"));
      child_argv.push_back(argv[2]);
      for (const char* flag : clean_flags)
        child_argv.push_back(const_cast<char*>(flag));
      child_argv.push_back(nullptr);
      ::execv(argv[1], child_argv.data());
      _exit(127);
    }
    int status = 0;
    return child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
  };

  const fs::path full_game = root / "games" / "clean-only-test";
  fs::create_directories(root / "config-root");
  fs::copy_file(argv[3], root / "config-root" / "baseline.yaml");
  fs::create_directories(full_game);
  std::ofstream(full_game / "seam_file.png") << "generated artifact\n";
  const bool full_clean_ok =
      run_clean("clean-only-test", {"--clean"}, nullptr) && !fs::exists(full_game / "seam_file.png");

  const fs::path partial_game = root / "games" / "partial-clean-only-test";
  fs::create_directories(partial_game);
  std::ofstream(partial_game / "left.png") << "synchronized left input\n";
  std::ofstream(partial_game / "right.png") << "synchronized right input\n";
  std::ofstream(partial_game / "seam_file.png") << "generated artifact\n";
  std::ofstream(partial_game / "config.yaml")
      << "game:\n  stitching:\n    frame_offsets:\n      left: 3\n      right: 0\n"
      << "hstream_ui:\n  stitching_calibration:\n    control_points: 750\n    status: pending\n"
      << "    stale_from: features\n"
      << "    artifacts_invalidated: false\n    invalidation_id: partial-clean-token\n";
  const bool partial_clean_ok =
      run_clean(
          "partial-clean-only-test",
          {"--clean-from-control-points", "--clean-expected-invalidation-id=partial-clean-token"},
          nullptr) &&
      !fs::exists(partial_game / "seam_file.png") && fs::exists(partial_game / "left.png") &&
      fs::exists(partial_game / "right.png");
  std::ifstream partial_config_input(partial_game / "config.yaml");
  const std::string partial_config(
      (std::istreambuf_iterator<char>(partial_config_input)), std::istreambuf_iterator<char>());
  const bool synchronization_preserved = partial_config.find("frame_offsets") != std::string::npos;

  const fs::path combined_game = root / "games" / "combined-clean-only-test";
  fs::create_directories(combined_game);
  std::ofstream(combined_game / "left.png") << "synchronized left input\n";
  std::ofstream(combined_game / "right.png") << "synchronized right input\n";
  std::ofstream(combined_game / "seam_file.png") << "generated artifact\n";
  const bool combined_clean_ok =
      run_clean("combined-clean-only-test", {"--clean", "--clean-from-control-points"}, nullptr) &&
      !fs::exists(combined_game / "seam_file.png") && !fs::exists(combined_game / "left.png") &&
      !fs::exists(combined_game / "right.png");
  const bool mismatched_runtime_token_rejected = !run_clean(
      "clean-only-test", {"--clean", "--clean-expected-invalidation-id=cli-token"}, "different-environment-token");
  const fs::path incomplete_malformed_config = root / "incomplete-malformed.yaml";
  std::ofstream(incomplete_malformed_config) << "application:\n  stage: -1\n  complete-configuration: 0\n"
                                             << "hmstitcher:\n  enable: 1\n  post-stitch-rotate-degrees: malformed\n";
  std::ofstream(full_game / "seam_file.png") << "generated artifact\n";
  const bool incomplete_context_skipped =
      run_clean("clean-only-test", {"--clean"}, nullptr, &incomplete_malformed_config) &&
      !fs::exists(full_game / "seam_file.png");
  const bool no_asset_download = !fs::exists(root / "home" / ".cache" / "hstream" / "models");
  fs::remove_all(root);
  if (!full_clean_ok || !partial_clean_ok || !synchronization_preserved || !combined_clean_ok ||
      !mismatched_runtime_token_rejected || !incomplete_context_skipped || !no_asset_download) {
    std::cerr << "FAIL: clean-only modes must respect dependency boundaries without downloading pretrained models\n";
    return 1;
  }
  return 0;
}
