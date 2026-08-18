#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;

bool exits_with_argument_error(
    const char* executable,
    const std::vector<std::string>& arguments,
    const fs::path& game_root = {},
    const std::string& control_points_environment = {}) {
  const pid_t child = ::fork();
  if (child == 0) {
    if (!game_root.empty()) {
      ::setenv("HM_GAME_DIR", game_root.c_str(), 1);
    }
    if (!control_points_environment.empty()) {
      ::setenv("HM_MAX_CONTROL_POINTS", control_points_environment.c_str(), 1);
    }
    std::vector<char*> args{const_cast<char*>(executable)};
    for (const std::string& argument : arguments) {
      args.push_back(const_cast<char*>(argument.c_str()));
    }
    args.push_back(nullptr);
    ::execv(executable, args.data());
    _exit(127);
  }
  int status = 0;
  return child > 0 && ::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 3;
}

} // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "FAIL: expected hstream-cli path\n";
    return 1;
  }
  for (const char* value : {
           "--stitch-frame-time=bogus",
           "--stitch-frame-time=00:00:07junk",
           "--stitch-frame-time=-1",
           "--stitch-frame-time=00:60:00",
           "--stitch-frame-time=inf",
       }) {
    if (!exits_with_argument_error(argv[1], {value})) {
      std::cerr << "FAIL: malformed stitch-frame time did not produce an invalid-argument exit: " << value << '\n';
      return 1;
    }
  }

  std::string pattern = (fs::temp_directory_path() / "hstream-stitch-frame-cli-test-XXXXXX").string();
  if (::mkdtemp(pattern.data()) == nullptr) {
    std::cerr << "FAIL: unable to create stitch-frame CLI test directory\n";
    return 1;
  }
  const fs::path root(pattern);
  const fs::path game_root = root / "games";
  const fs::path game_dir = game_root / "malformed-config";
  const fs::path conflict_game_dir = game_root / "control-point-conflict";
  const fs::path pipeline_config = root / "pipeline.yaml";
  const fs::path rotation_zero_config = root / "rotation-zero.yaml";
  const fs::path rotation_ten_config = root / "rotation-ten.yaml";
  fs::create_directories(game_dir);
  fs::create_directories(conflict_game_dir);
  std::ofstream(game_dir / "config.yaml") << "stitching:\n  stitch_frame_time:\n    - 00:00:07\n";
  std::ofstream(pipeline_config) << "application:\n  stage: 0\n";
  const bool malformed_config_rejected = exits_with_argument_error(
      argv[1], {"--game-id=malformed-config", "--cfg-file=" + pipeline_config.string()}, game_root);
  if (!malformed_config_rejected) {
    std::cerr << "FAIL: a non-scalar config-file stitch-frame time did not produce an invalid-argument exit\n";
    fs::remove_all(root);
    return 1;
  }
  std::ofstream(conflict_game_dir / "config.yaml") << "pipeline:\n"
                                                   << "  application:\n"
                                                   << "    complete-configuration: 1\n"
                                                   << "  hmstitcher:\n"
                                                   << "    enable: 1\n"
                                                   << "hstream_ui:\n"
                                                   << "  stitching_calibration:\n"
                                                   << "    control_points: 750\n"
                                                   << "    status: pending\n"
                                                   << "    stale_from: input\n"
                                                   << "    artifacts_invalidated: true\n"
                                                   << "    invalidation_id: saved-control-points\n";
  if (!exits_with_argument_error(
          argv[1],
          {"--game-id=control-point-conflict", "--cfg-file=" + pipeline_config.string()},
          game_root,
          /*control_points_environment=*/"1500")) {
    std::cerr << "FAIL: a runtime control-point limit conflicting with the pending saved generation was not rejected\n";
    fs::remove_all(root);
    return 1;
  }
  std::ofstream(rotation_zero_config)
      << "application:\n  stage: 0\nhmstitcher:\n  enable: 1\n  post-stitch-rotate-degrees: 0\n";
  std::ofstream(rotation_ten_config)
      << "application:\n  stage: 0\nhmstitcher:\n  enable: 1\n  post-stitch-rotate-degrees: 10\n";
  if (!exits_with_argument_error(
          argv[1],
          {"--cfg-file=" + rotation_zero_config.string(), "--cfg-file=" + rotation_ten_config.string()},
          game_root)) {
    std::cerr << "FAIL: same-stage stitchers with incompatible output rotations were not rejected\n";
    fs::remove_all(root);
    return 1;
  }
  fs::remove_all(root);
  return 0;
}
