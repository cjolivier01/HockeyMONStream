#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "yaml-cpp/yaml.h"

namespace fs = std::filesystem;

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

bool write(const fs::path& path, const std::string& contents) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path);
  output << contents;
  return output.good();
}

} // namespace

int main() {
  char root_template[] = "/tmp/hstream-stitch-experiment-backend-XXXXXX";
  const char* created = ::mkdtemp(root_template);
  if (!created)
    return 1;
  const fs::path root(created);
  struct Cleanup {
    fs::path root;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(root, ignored);
    }
  } cleanup{root};

  const fs::path game = root / "game";
  const fs::path experiments = root / "experiments";
  if (!write(game / "cam1" / "left.mp4", "left") || !write(game / "cam2" / "right.mp4", "right") ||
      !write(game / "program-output.mp4", "not a camera input") || !write(game / "left_calibration.json", "{}") ||
      !write(game / "cam2" / "right-calibration.yaml", "camera: right") ||
      !write(
          game / "config.yaml",
          "game:\n"
          "  videos:\n"
          "    left: [cam1/left.mp4]\n"
          "    right: [cam2/right.mp4]\n"
          "  stitching:\n"
          "    control_points: [old-game-cache]\n"
          "stitching:\n"
          "  control_points: [old-cache]\n"
          "  mapping_backend: nona\n"
          "  projection: general-panini\n"
          "  projection_framing:\n"
          "    rotation_degrees: [7, -2, 1]\n"
          "rink:\n"
          "  scoreboard:\n"
          "    perspective_polygon: [[1, 2], [3, 4]]\n"
          "  ice_contours_mask_count: 2\n"
          "  ice_contours_mask_centroid: [10, 20]\n"
          "  ice_contours_combined_bbox: [0, 0, 100, 50]\n"
          "  stitched_output_pending_previous_generation: old\n"
          "  stitched_output_pending_previous_authorization_id: auth\n"
          "  stitched_output_pending_previous_owner_process: 123\n"
          "  stitched_output_pending_completed_scoreboard_polygon: [[5, 6], [7, 8]]\n")) {
    return 2;
  }

  StitchingExperimentSettings settings{
      .control_points = 900,
      .frame_count = 4,
      .stitch_frame_time = "00:01:02.500",
      .rink_rotation_degrees = std::array<double, 3>{0.0, -1.5, 0.5},
  };
  const auto workspace = CreateStitchingExperimentWorkspace(game, experiments, settings, 1);
  if (!expect(workspace.ok(), "candidate workspace must be created")) {
    if (!workspace.ok())
      std::cerr << workspace.status() << '\n';
    return 3;
  }

  bool ok = true;
  ok &= expect(fs::is_symlink(workspace->game_directory / "cam1" / "left.mp4"), "left video must be linked");
  ok &= expect(fs::is_symlink(workspace->game_directory / "cam2" / "right.mp4"), "right video must be linked");
  ok &= expect(
      !fs::exists(workspace->game_directory / "program-output.mp4"),
      "unconfigured Program output must not become an experiment camera input");
  ok &= expect(
      fs::is_symlink(workspace->game_directory / "left_calibration.json"), "root camera calibration must be linked");
  ok &= expect(
      fs::is_symlink(workspace->game_directory / "cam2" / "right-calibration.yaml"),
      "camera-directory calibration must be linked");

  const fs::path outside = root / "outside.mp4";
  if (!write(outside, "outside"))
    return 4;
  std::error_code symlink_error;
  fs::create_symlink(outside, game / "cam1" / "escaped.mp4", symlink_error);
  const auto escaped_workspace = CreateStitchingExperimentWorkspace(game, experiments, settings, 2);
  ok &= expect(
      !escaped_workspace.ok() && absl::IsInvalidArgument(escaped_workspace.status()),
      "auto-discovered video symlinks must not escape the selected game");

  const YAML::Node config = YAML::LoadFile((workspace->game_directory / "config.yaml").string());
  const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  const YAML::Node rotation = config["stitching"]["projection_framing"]["rotation_degrees"];
  ok &= expect(calibration["control_points"].as<int>() == 900, "candidate control-point count must be saved");
  ok &= expect(calibration["frame_count"].as<int>() == 4, "candidate frame count must be saved");
  ok &= expect(calibration["status"].as<std::string>() == "pending", "candidate calibration must be pending");
  ok &= expect(
      config["stitching"]["stitch_frame_time"].as<std::string>() == "00:01:02.500",
      "candidate first frame must be saved");
  ok &= expect(
      rotation.size() == 3 && rotation[0].as<double>() == 7.0 && rotation[1].as<double>() == -1.5 &&
          rotation[2].as<double>() == 0.5,
      "candidate rink pitch and roll must override the saved setting while preserving yaw");
  ok &= expect(
      calibration["backend_generation"]["invalidation_id"].as<std::string>() == workspace->invalidation_id,
      "candidate backend generation must be fenced by its invalidation id");
  const auto selected_config =
      BuildStitchingExperimentSelectionConfig(workspace->game_directory / "config.yaml", game / "config.yaml");
  ok &= expect(selected_config.ok(), "selected candidate config must be mergeable");
  if (selected_config.ok()) {
    const YAML::Node selected = YAML::Load(*selected_config);
    const YAML::Node selected_rink = selected["rink"];
    ok &= expect(
        !selected["stitching"]["control_points"].IsDefined() &&
            !selected["game"]["stitching"]["control_points"].IsDefined() &&
            !selected_rink["scoreboard"]["perspective_polygon"].IsDefined() &&
            !selected_rink["ice_contours_mask_count"].IsDefined() &&
            !selected_rink["ice_contours_mask_centroid"].IsDefined() &&
            !selected_rink["ice_contours_combined_bbox"].IsDefined() &&
            !selected_rink["stitched_output_pending_previous_generation"].IsDefined() &&
            !selected_rink["stitched_output_pending_previous_authorization_id"].IsDefined() &&
            !selected_rink["stitched_output_pending_previous_owner_process"].IsDefined() &&
            !selected_rink["stitched_output_pending_completed_scoreboard_polygon"].IsDefined(),
        "selection must remove every geometry-dependent rink cache and pending recovery field");
  }
  return ok ? 0 : 5;
}
