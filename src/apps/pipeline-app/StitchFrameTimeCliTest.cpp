#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

namespace {

namespace fs = std::filesystem;

int run_and_get_exit_code(
    const char* executable,
    const std::vector<std::string>& arguments,
    const fs::path& game_root = {},
    const std::string& control_points_environment = {}) {
  const pid_t child = ::fork();
  if (child == 0) {
    if (!game_root.empty()) {
      ::setenv("HM_GAME_DIR", game_root.c_str(), 1);
    } else {
      ::unsetenv("HM_GAME_DIR");
    }
    if (!control_points_environment.empty()) {
      ::setenv("HM_MAX_CONTROL_POINTS", control_points_environment.c_str(), 1);
    } else {
      ::unsetenv("HM_MAX_CONTROL_POINTS");
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
  if (child <= 0 || ::waitpid(child, &status, 0) != child || !WIFEXITED(status))
    return -1;
  return WEXITSTATUS(status);
}

bool exits_with_argument_error(
    const char* executable,
    const std::vector<std::string>& arguments,
    const fs::path& game_root = {},
    const std::string& control_points_environment = {}) {
  return run_and_get_exit_code(executable, arguments, game_root, control_points_environment) == 3;
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
  const fs::path configured_game_dir = game_root / "configured-game";
  const fs::path pipeline_config = root / "pipeline.yaml";
  const fs::path nonstitch_pipeline_config = root / "nonstitch-pipeline.yaml";
  const fs::path disabled_stitch_pipeline_config = root / "disabled-stitch-pipeline.yaml";
  const fs::path configured_pipeline_config = root / "configured-pipeline.yaml";
  const fs::path rotation_zero_config = root / "rotation-zero.yaml";
  const fs::path rotation_ten_config = root / "rotation-ten.yaml";
  fs::create_directories(game_dir);
  fs::create_directories(conflict_game_dir);
  fs::create_directories(configured_game_dir);
  std::ofstream(game_dir / "config.yaml") << "stitching:\n  stitch_frame_time:\n    - 00:00:07\n";
  std::ofstream(pipeline_config) << "application:\n  stage: 0\n";
  std::ofstream(nonstitch_pipeline_config) << "application:\n"
                                           << "  stage: 0\n"
                                           << "  complete-configuration: 1\n"
                                           << "primary-gie:\n"
                                           << "  enable: 1\n"
                                           << "  config-file: deliberately-missing-model-config.txt\n";
  std::ofstream(disabled_stitch_pipeline_config) << "application:\n"
                                                 << "  stage: 0\n"
                                                 << "  complete-configuration: 1\n"
                                                 << "hmstitcher:\n"
                                                 << "  enable: 0\n";
  std::ofstream(configured_pipeline_config) << "application:\n"
                                            << "  stage: 0\n"
                                            << "  complete-configuration: 1\n"
                                            << "hmstitcher:\n"
                                            << "  enable: 1\n";
  const bool malformed_config_rejected = exits_with_argument_error(
      argv[1], {"--game-id=malformed-config", "--cfg-file=" + pipeline_config.string()}, game_root);
  if (!malformed_config_rejected) {
    std::cerr << "FAIL: a non-scalar config-file stitch-frame time did not produce an invalid-argument exit\n";
    fs::remove_all(root);
    return 1;
  }
  std::ofstream(game_dir / "config.yaml") << "stitching:\n  stitch_frame_time: \"\"\n";
  if (!exits_with_argument_error(
          argv[1], {"--game-id=malformed-config", "--cfg-file=" + pipeline_config.string()}, game_root)) {
    std::cerr << "FAIL: an explicitly empty config-file stitch-frame time did not produce an invalid-argument exit\n";
    fs::remove_all(root);
    return 1;
  }
  std::ofstream(configured_game_dir / "config.yaml") << "stitching:\n"
                                                     << "  stitch_frame_time: 00:00:03\n"
                                                     << "hstream_ui:\n"
                                                     << "  stitching_calibration:\n"
                                                     << "    control_points: 750\n"
                                                     << "    status: complete\n"
                                                     << "    invalidation_id: configured-owner\n";
  const int configured_nonzero_exit = run_and_get_exit_code(
      argv[1],
      {"--game-id=configured-game",
       "--cfg-file=" + nonstitch_pipeline_config.string(),
       "--cfg-file=" + disabled_stitch_pipeline_config.string(),
       "--cfg-file=" + configured_pipeline_config.string(),
       "--cfg-file=" + configured_pipeline_config.string(),
       "--stitch-frame-time=00:00:07",
       "--clean"},
      game_root);
  if (configured_nonzero_exit != 0) {
    std::cerr << "FAIL: configured-game nonzero stitch-frame override did not complete clean-only setup\n";
    fs::remove_all(root);
    return 1;
  }
  YAML::Node configured_after_nonzero = YAML::LoadFile((configured_game_dir / "config.yaml").string());
  const YAML::Node nonzero_calibration = configured_after_nonzero["hstream_ui"]["stitching_calibration"];
  const std::string nonzero_owner = nonzero_calibration["invalidation_id"].as<std::string>("");
  if (configured_after_nonzero["stitching"]["stitch_frame_time"].as<std::string>("") != "00:00:07" ||
      nonzero_calibration["status"].as<std::string>("") != "pending" ||
      nonzero_calibration["stale_from"].as<std::string>("") != "input" ||
      nonzero_calibration["artifacts_invalidated"].as<bool>(true) || nonzero_owner.empty() ||
      nonzero_owner == "configured-owner") {
    std::cerr << "FAIL: configured-game nonzero CLI override was not persisted and invalidated from input\n"
              << YAML::Dump(configured_after_nonzero) << '\n';
    fs::remove_all(root);
    return 1;
  }
  const int configured_zero_exit = run_and_get_exit_code(
      argv[1],
      {"--game-id=configured-game",
       "--cfg-file=" + configured_pipeline_config.string(),
       "--cfg-file=" + nonstitch_pipeline_config.string(),
       "--cfg-file=" + disabled_stitch_pipeline_config.string(),
       "--cfg-file=" + configured_pipeline_config.string(),
       "--stitch-frame-time=00:00:00",
       "--clean"},
      game_root);
  if (configured_zero_exit != 0) {
    std::cerr << "FAIL: configured-game zero stitch-frame override did not complete clean-only setup\n";
    fs::remove_all(root);
    return 1;
  }
  YAML::Node configured_after_zero = YAML::LoadFile((configured_game_dir / "config.yaml").string());
  const YAML::Node zero_calibration = configured_after_zero["hstream_ui"]["stitching_calibration"];
  const std::string zero_owner = zero_calibration["invalidation_id"].as<std::string>("");
  if (configured_after_zero["stitching"]["stitch_frame_time"] ||
      zero_calibration["status"].as<std::string>("") != "pending" ||
      zero_calibration["stale_from"].as<std::string>("") != "input" ||
      zero_calibration["artifacts_invalidated"].as<bool>(true) || zero_owner.empty() || zero_owner == nonzero_owner) {
    std::cerr << "FAIL: configured-game zero CLI override was not removed and invalidated from input\n"
              << YAML::Dump(configured_after_zero) << '\n';
    fs::remove_all(root);
    return 1;
  }
  const int no_eligible_exit = run_and_get_exit_code(
      argv[1],
      {"--game-id=configured-game",
       "--cfg-file=" + nonstitch_pipeline_config.string(),
       "--cfg-file=" + disabled_stitch_pipeline_config.string(),
       "--clean"},
      game_root);
  if (no_eligible_exit == 0 || no_eligible_exit == 127) {
    std::cerr << "FAIL: clean-only setup without an active eligible stitcher was not rejected\n";
    fs::remove_all(root);
    return 1;
  }
  std::ofstream(configured_game_dir / "config.yaml") << "stitching:\n"
                                                     << "  stitch_frame_time: 00:00:03\n"
                                                     << "hstream_ui:\n"
                                                     << "  stitching_calibration:\n"
                                                     << "    control_points: 750\n"
                                                     << "    status: pending\n"
                                                     << "    stale_from: input\n"
                                                     << "    artifacts_invalidated: false\n"
                                                     << "    invalidation_id: current-owner\n";
  const int superseded_exit = run_and_get_exit_code(
      argv[1],
      {"--game-id=configured-game",
       "--cfg-file=" + nonstitch_pipeline_config.string(),
       "--cfg-file=" + configured_pipeline_config.string(),
       "--stitch-frame-time=00:00:07",
       "--clean",
       "--clean-expected-invalidation-id=stale-owner"},
      game_root);
  if (superseded_exit <= 0 || superseded_exit == 127) {
    std::cerr << "FAIL: a superseded guarded stitch-frame override was not rejected\n";
    fs::remove_all(root);
    return 1;
  }
  const YAML::Node configured_after_superseded = YAML::LoadFile((configured_game_dir / "config.yaml").string());
  const YAML::Node superseded_calibration = configured_after_superseded["hstream_ui"]["stitching_calibration"];
  if (configured_after_superseded["stitching"]["stitch_frame_time"].as<std::string>("") != "00:00:03" ||
      superseded_calibration["status"].as<std::string>("") != "pending" ||
      superseded_calibration["invalidation_id"].as<std::string>("") != "current-owner") {
    std::cerr << "FAIL: a superseded guarded override mutated game-private stitch-frame state\n"
              << YAML::Dump(configured_after_superseded) << '\n';
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
