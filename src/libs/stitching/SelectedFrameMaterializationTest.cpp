#include <opencv2/imgcodecs.hpp>
#include <unistd.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/stitching/AutoInputLens.h"
#include "hstream/src/libs/stitching/ConfigureStitchingCore.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"

namespace fs = std::filesystem;
using namespace hm::stitching;
namespace {
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void check(const absl::Status& status) {
  require(status.ok(), status.ToString());
}
template <class T>
T take(absl::StatusOr<T> value) {
  check(value.status());
  return std::move(*value);
}
} // namespace
int main(int argc, char** argv) {
  char temporary[] = "/tmp/hstream-materialize-test-XXXXXX";
  const auto created = ::mkdtemp(temporary);
  if (!created)
    return 1;
  const bool prepare = argc == 5 && std::string(argv[1]) == "--prepare-only";
  const fs::path game = prepare ? fs::absolute(argv[2]) : fs::path(created);
  if (prepare) {
    fs::remove(created);
    fs::create_directories(game);
  }
  struct Cleanup {
    fs::path path;
    bool retain;
    ~Cleanup() {
      if (!retain)
        fs::remove_all(path);
    }
  } cleanup{game, prepare};
  const uint64_t anchor = prepare ? 0 : 10 * kPlayerFrameSecond;
  try {
    fs::create_directory(game / "_hockeymon");
    auto config = YAML::Clone(take(hm::baseline_config::load()).values);
    config["stitching"]["control_point_execution_provider"] = "cpu";
    config["stitching"]["control_point_matcher"] = "superpoint-lightglue";
    config["stitching"]["calibration_frame_count"] = 2;
    config["stitching"]["stitch_frame_time"] = prepare ? "00:00:00" : "00:00:10";
    auto calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "pending";
    calibration["invalidation_id"] = "materialize-without-model";
    calibration["stale_from"] = "features";
    calibration["artifacts_invalidated"] = true;
    calibration["frame_count"] = 2;
    calibration["control_points"] = take(read_control_point_limit(config));
    PlayerFrameSelectionPlan plan;
    plan.settings.frame_count = 2;
    for (const char* role : {"left", "right"}) {
      const auto source = game / (std::string(role) + ".mp4");
      if (prepare)
        fs::copy_file(argv[std::string(role) == "left" ? 3 : 4], source);
      else
        std::ofstream(source) << "source identity fixture";
      plan.sources.push_back(take(BindPlayerFrameSource(source)));
      // Production experiment workspaces use local aliases. Keep those same
      // names as successors move through independent attempt directories.
      config["game"]["videos"][role].push_back(source.filename().string());
      config["game"]["stitching"]["frame_offsets"][role] = 0.0;
    }
    if (prepare) {
      auto camera = ReadAutoInputLensForGame(game, config, take(read_stitch_camera_selection(config))).camera;
      for (auto& [source, values] : camera.input_cameras)
        values.frozen = true;
      write_stitch_camera_selection(config, camera);
      config["stitching"]["auto_input_lens_frozen"] = camera.has_automatic_optics();
    }
    for (const char* key :
         {"baseline_generation",
          "output_generation",
          "detector_identity",
          "rink_mask_sha256",
          "rink_mask_revision",
          "fieldmask_settings",
          "output_rotation_degrees"})
      plan.context[key] = "fixture";
    plan.context["decode_anchor_ns"] = std::to_string(anchor);
    plan.context["source_context"] = take(player_frame_source_context(config, anchor));
    for (uint64_t index = 0; index < 2; ++index) {
      PlayerFrameObservation observation;
      observation.pair.timeline_pts_ns = anchor + index * kPlayerFrameSecond;
      for (size_t camera = 0; camera < 2; ++camera)
        observation.pair.cameras[camera] = {
            plan.sources[camera].path, observation.pair.timeline_pts_ns, static_cast<uint32_t>(camera), index};
      observation.eligible_people = 1;
      observation.size_band_counts = {1, 0, 0};
      observation.coverage = {0};
      observation.quality = 1;
      plan.selected.push_back(observation);
    }
    plan.fingerprint = take(PlayerFrameSelectionFingerprint(plan));
    config["stitching"]["calibration_frame_selection"] = PlayerFrameSelectionPlanYaml(plan);
    const auto choices = take(read_stitching_backend_choices(config));
    check(reserve_stitching_backend_generation_in_config(config, "materialize-without-model", choices));
    std::ofstream(game / "_hockeymon/config.yaml") << YAML::Dump(config);
    if (prepare) {
      std::cout << game << "\n";
      return 0;
    }
    ::setenv("HM_FEATURE_MATCHER_CPU_ONNX_MODEL", "/missing/materialization-must-not-open.onnx", 1);
    ::setenv("HM_EXIFTOOL", "/missing/exiftool", 1);
    int writes = 0;
    std::vector<CalibrationFrameInput> frames;
    for (int index = 0; index < 2; ++index) {
      const auto writer = [&](const fs::path& path) {
        ++writes;
        return cv::imwrite(path.string(), cv::Mat(24, 32, CV_8UC3, cv::Scalar(20, 40, 60)))
            ? absl::OkStatus()
            : absl::InternalError("PNG writer failed");
      };
      frames.push_back(
          {writer,
           writer,
           {plan.sources[0].path, double(10 + index)},
           {plan.sources[1].path, double(10 + index)},
           {},
           {}});
    }
    check(configure_stitching(game.string(), frames, "materialize-without-model", {}, 0, plan.fingerprint, true));
    require(writes == 4, "Materialization must invoke each bounded image writer exactly once");
    auto retained = take(LoadPlayerFrameInputs(game, plan));
    require(retained && retained->images.size() == 2, "Complete retained PNG bundle is missing");
    for (const auto& pair : retained->images)
      for (const auto& image : pair)
        require(!cv::imread(image.string()).empty(), "Published image is unreadable");
    const auto saved = YAML::LoadFile((game / "_hockeymon/config.yaml").string());
    require(
        saved["hstream_ui"]["stitching_calibration"]["status"].as<std::string>() == "pending",
        "Materialization must not complete calibration");
    require(!fs::exists(game / "_hockeymon/hm_project.pto"), "Materialization must not run the solver");
    for (size_t i = 0; i < frames.size(); ++i) {
      frames[i].left_image = retained->images[i][0];
      frames[i].right_image = retained->images[i][1];
    }
    const auto failed =
        configure_stitching(game.string(), frames, "materialize-without-model", {}, 0, plan.fingerprint);
    require(!failed.ok(), "Solve with an unavailable model must fail independently");
    require(
        writes == 4 && take(LoadPlayerFrameInputs(game, plan)).has_value(),
        "Failed matching must preserve the successfully materialized bundle without recapture");
    std::cout << "Materialization succeeds without matcher, preserves pending calibration and survives failed solve\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
