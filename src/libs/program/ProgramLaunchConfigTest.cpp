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
    Check(
        result->game == game_dir && result->chapters[0].size() == 2 &&
            result->chapters[0][1] == game_dir / "left1.mp4" && result->frame_offsets[0] == 2.5,
        "Saved chapters or fractional synchronization changed");
    Check(result->camera["zoom-in-aggressiveness"].as<int>() == 61, "Game camera tuning did not override user tuning");
    auto overridden = load({"rink.camera.zoom_in_aggressiveness=83"});
    Check(
        overridden.ok() && overridden->camera["zoom-in-aggressiveness"].as<int>() == 83,
        "Launch override did not reach the Program camera");
    Check(
        overridden->saved_game["rink"]["camera"]["zoom_in_aggressiveness"].as<int>() == 61 &&
            YAML::Dump(overridden->saved_game) == YAML::Dump(game),
        "Launch overrides changed the saved game snapshot used for calibration admission");

    // The AMD launch path uses the same editable defaults overlay as NVIDIA.
    const auto defaults_path = fixture.root / ".config/hstream-ui/defaults.yaml";
    fs::create_directories(defaults_path.parent_path());
    game["hstream_ui"]["stitching_calibration"]["frame_count"] = 4;
    Write(game_dir / "config.yaml", game);
    user["pipeline"]["hmstitcher"]["blend-mode"] = "gpu-hard-seam";
    Write(user_dir / "hstream.yaml", user);
    Write(
        defaults_path,
        YAML::Load(
            "rink: {camera: {zoom_in_aggressiveness: 48}}\n"
            "stitching: {blend_mode: alpha, blend_feather_fraction: 0.3, calibration_frame_count: 6}"));
    result = load();
    Check(
        result.ok() && result->camera["zoom-in-aggressiveness"].as<int>() == 61 && result->blend.mode == "alpha" &&
            result->configuration.values["stitching"]["calibration_frame_count"].as<int>() == 4,
        "Game must override Defaults and old native aliases must not override Defaults on AMD");
    game["rink"]["camera"].remove("zoom_in_aggressiveness");
    Write(game_dir / "config.yaml", game);
    result = load();
    Check(
        result.ok() && result->camera["zoom-in-aggressiveness"].as<int>() == 48,
        "AMD camera must inherit UI defaults above legacy preferences");
    auto defaults_camera = YAML::LoadFile(argv[3]);
    defaults_camera["play-tracker"]["zoom-in-aggressiveness"] = 29;
    Write(defaults_path.parent_path() / "default-camera.yaml", defaults_camera);
    Write(defaults_path, YAML::Load("pipeline: {ds-playtracker: {config-file: default-camera.yaml}}"));
    result = load();
    Check(
        result.ok() && result->camera["zoom-in-aggressiveness"].as<int>() == 29,
        "Native Defaults references must resolve beside defaults.yaml");
    Write(defaults_path, YAML::Load("{}"));
    result = load();
    Check(
        result.ok() &&
            result->camera["zoom-in-aggressiveness"].as<int>() ==
                YAML::LoadFile(argv[1])["rink"]["camera"]["zoom_in_aggressiveness"].as<int>(),
        "Empty defaults must restore baseline tuning, not legacy tuning");
    Write(defaults_path, YAML::Load("[]"));
    Check(!load().ok(), "Invalid UI defaults must fail closed on AMD");
    fs::remove(defaults_path);
    user["pipeline"].remove("hmstitcher");
    Write(user_dir / "hstream.yaml", user);
    game["rink"]["camera"]["zoom_in_aggressiveness"] = 61;
    Write(game_dir / "config.yaml", game);

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
    Check(
        result->camera["zoom-in-aggressiveness"].as<int>() == 29 &&
            result->inference_file == game_dir / "custom-infer.yaml" &&
            result->inference["property"]["network-mode"].as<int>() == 2,
        "Game-relative native selection was ignored");
    overridden = load({"rink.camera.zoom_in_aggressiveness=83"});
    Check(
        overridden.ok() && overridden->camera["zoom-in-aggressiveness"].as<int>() == 83,
        "Higher canonical layer did not override the selected native camera");
    game["pipeline"]["hmstitcher"]["blend-mode"] = "laplacian";
    game["pipeline"]["hmstitcher"]["blend-feather-fraction"] = 0.12;
    game["pipeline"]["hmstitcher"]["minimize-blend"] = 0;
    Write(game_dir / "config.yaml", game);
    result = load();
    Check(result.ok() && !result->minimize_blend, "Saved native minimize-blend was ignored");
    overridden = load({"stitching.minimize_blend=true"});
    Check(overridden.ok() && overridden->minimize_blend, "Lower native minimize-blend overrode launch choice");
    overridden = load({"stitching.minimize_blend=true", "pipeline.hmstitcher.minimize-blend=0"});
    Check(overridden.ok() && !overridden->minimize_blend, "Native minimize-blend lost same-layer precedence");
    overridden = load({"stitching.blend_mode=alpha,stitching.blend_feather_fraction=0.3"});
    Check(
        overridden.ok() && overridden->blend.mode == "alpha" && overridden->blend.feather_fraction == 0.3,
        "Lower native blend alias overrode the launch layer");
    overridden = load({"stitching.blend_mode=alpha,pipeline.hmstitcher.blend-mode=hard-seam"});
    Check(overridden.ok() && overridden->blend.mode == "gpu-hard-seam", "Native blend lost its same-layer precedence");
    overridden = load({"pipeline.hmstitcher.blend-mode=gpu-hard-seam", "stitching.blend_mode=alpha"});
    Check(
        overridden.ok() && overridden->blend.mode == "gpu-hard-seam",
        "Separate launch groups changed native alias precedence");
    Check(
        !load({"pipeline.primary-gie.config-file=/missing/explicit-infer.yaml"}).ok(),
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
