#include "hstream/src/libs/program/ProgramLaunchConfig.h"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
void Check(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void Write(const fs::path& path, const YAML::Node& value) {
  std::ofstream output(path);
  output << value << '\n';
  Check(output.good(), "Cannot write fixture");
}
struct Fixture {
  fs::path root;
  Fixture() {
    std::string name = (fs::temp_directory_path() / "hstream-launch-config-XXXXXX").string();
    Check(mkdtemp(name.data()), "Cannot create fixture");
    root = name;
  }
  ~Fixture() {
    std::error_code ignored;
    fs::remove_all(root, ignored);
  }
};
} // namespace

int main(int argc, char** argv) {
  try {
    Check(argc == 5, "Expected baseline, application, camera and inference YAMLs");
    Fixture fixture;
    const auto config_root = fixture.root / "configs", game_root = fixture.root / "games";
    const auto game_dir = game_root / "game", user_dir = fixture.root / ".hstream";
    for (const auto& directory : {config_root, game_dir, user_dir})
      fs::create_directories(directory);
    fs::copy_file(argv[1], config_root / "baseline.yaml");
    fs::copy_file(argv[2], config_root / "app.yaml");
    fs::copy_file(argv[3], config_root / "play_tracker_config.yaml");
    fs::copy_file(argv[4], config_root / "config_infer_yolov8_hockey.yaml");
    setenv("HOME", fixture.root.c_str(), 1);
    setenv("HM_CONFIG_ROOT", config_root.c_str(), 1);
    unsetenv("HM_GAME_DIR");
    YAML::Node user = YAML::Load("rink: {camera: {zoom_in_aggressiveness: 37}}");
    user["paths"]["game-root"] = game_root.string();
    Write(user_dir / "hstream.yaml", user);
    YAML::Node game = YAML::Load(
        "game:\n  videos: {left: [left0.mp4, left1.mp4], right: [right.mp4]}\n"
        "  stitching: {frame_offsets: {left: 2.5, right: 0}}\n"
        "rink: {camera: {zoom_in_aggressiveness: 61}}\n");
    Write(game_dir / "config.yaml", game);
    const auto load = [&](const std::vector<std::string>& options = {}) {
      return hm::program::LoadProgramLaunchConfig("game", config_root / "app.yaml", options);
    };
    auto result = load();
    Check(result.ok(), result.status().ToString());
    Check(result->game == game_dir && result->chapters[0].size() == 2 &&
              result->chapters[0][1] == game_dir / "left1.mp4" && result->frame_offsets[0] == 2.5,
          "Saved chapters or fractional synchronization changed");
    Check(result->camera["zoom-in-aggressiveness"].as<int>() == 61,
          "Game camera tuning did not override user tuning");
    auto overridden = load({"rink.camera.zoom_in_aggressiveness=83"});
    Check(overridden.ok() && overridden->camera["zoom-in-aggressiveness"].as<int>() == 83,
          "Launch override did not reach the Program camera");

    // Explicit native files preserve same-layer camera values. Higher launch
    // canonical values still win, using the shared NVIDIA mapping contract.
    auto custom_camera = YAML::LoadFile(argv[3]);
    custom_camera["play-tracker"]["zoom-in-aggressiveness"] = 29;
    Write(game_dir / "custom-camera.yaml", custom_camera);
    game["pipeline"]["ds-playtracker"]["config-file"] = "custom-camera.yaml";
    auto custom_inference = YAML::LoadFile(argv[4]);
    custom_inference["property"]["network-mode"] = 2;
    Write(game_dir / "custom-infer.yaml", custom_inference);
    game["pipeline"]["primary-gie"]["config-file"] = "custom-infer.yaml";
    Write(game_dir / "config.yaml", game);
    result = load();
    Check(result.ok(), result.status().ToString());
    Check(result->camera["zoom-in-aggressiveness"].as<int>() == 29 &&
              result->inference_file == game_dir / "custom-infer.yaml" &&
              result->inference["property"]["network-mode"].as<int>() == 2,
          "Game-relative native selection was ignored");
    overridden = load({"rink.camera.zoom_in_aggressiveness=83"});
    Check(overridden.ok() && overridden->camera["zoom-in-aggressiveness"].as<int>() == 83,
          "Higher canonical layer did not override the selected native camera");
    Check(!load({"pipeline.primary-gie.config-file=/missing/explicit-infer.yaml"}).ok(),
          "Missing explicit inference selection fell back");

    // Older calibrations store synchronization in the top-level section; null
    // in the newer location must not hide that saved data.
    game["stitching"]["frame_offsets"] = YAML::Clone(game["game"]["stitching"]["frame_offsets"]);
    game["game"]["stitching"]["frame_offsets"] = YAML::Load("{left: null, right: null}");
    Write(game_dir / "config.yaml", game);
    result = load();
    Check(result.ok() && result->frame_offsets[0] == 2.5, "Legacy synchronization was not loaded");
    for (const auto* bad : {"-.1", ".nan", ".inf", "null", "[]"}) {
      game["stitching"]["frame_offsets"]["left"] = YAML::Load(bad);
      Write(game_dir / "config.yaml", game);
      Check(!load().ok(), "Invalid synchronization accepted");
    }
    game["stitching"]["frame_offsets"]["left"] = 0;
    game["game"]["videos"]["right"] = YAML::Load("[]");
    Write(game_dir / "config.yaml", game);
    Check(!load().ok(), "Missing saved chapters accepted");
    setenv("HM_CONFIG_ROOT", (fixture.root / "absent").c_str(), 1);
    Check(!load().ok(), "Invalid explicit config root fell back");
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
