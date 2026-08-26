#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"

#include <yaml-cpp/yaml.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <unistd.h>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

void write_config(const std::filesystem::path& path, const std::string& generation) {
  YAML::Node config(YAML::NodeType::Map);
  config["unrelated"] = "preserved";
  config["rink"]["stitched_output_generation"] = generation;
  config["rink"]["scoreboard"]["perspective_polygon"] = std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}};
  std::ofstream(path) << config << '\n';
}

absl::StatusOr<std::string> create_hugin_generation(const std::filesystem::path& root) {
  for (const char* name : {
           "hm_project.pto",
           "autooptimiser_out.pto",
           "mapping_0000.tif",
           "mapping_0000_x.tif",
           "mapping_0000_y.tif",
           "mapping_0001.tif",
           "mapping_0001_x.tif",
           "mapping_0001_y.tif",
           "seam_file.png",
       }) {
    std::ofstream(root / name) << name << '\n';
  }
  auto lock = hm::stitching::lock_canvas_constraint_artifacts(root);
  if (!lock.ok())
    return lock.status();
  return hm::stitching::stitch_artifact_generation_id_locked(root);
}

} // namespace

int main() {
  namespace fs = std::filesystem;
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("live-stitching-generation-test-" + std::to_string(::getpid()));
  fs::create_directories(root);

  const auto current_hugin_generation = create_hugin_generation(root);
  ok &= expect(current_hugin_generation.ok(), "live generation fixture must publish identifiable Hugin artifacts");
  if (!current_hugin_generation.ok()) {
    fs::remove_all(root);
    return 1;
  }
  const std::string hugin_generation = *current_hugin_generation;
  const std::string generation = "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin_generation.size()) +
      "\n" + hugin_generation + "post-stitch-rotate-degrees:0\noutput-size:320x180\n";
  const std::string rotated_generation =
      "hstream-stitched-output-v1\nhugin-bytes:" + std::to_string(hugin_generation.size()) + "\n" + hugin_generation +
      "post-stitch-rotate-degrees:9.25\noutput-size:320x180\n";
  write_config(root / "config.yaml", generation);

  const auto authorize = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25, "auth-a");
  YAML::Node config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      authorize.ok() && authorize->pending_generation == rotated_generation &&
          config["unrelated"].as<std::string>() == "preserved" &&
          config["rink"]["stitched_output_generation"].as<std::string>() == generation &&
          config["rink"]["stitched_output_pending_generation"].as<std::string>() == rotated_generation &&
          config["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
      "authorization must preserve generation bytes and defer active scoreboard invalidation");

  const auto restore = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 0.0, "auth-restore");
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      restore.ok() && restore->pending_generation == generation &&
          restore->scoreboard_property_value == "1,2,3,4,5,6,7,8" &&
          config["rink"]["stitched_output_pending_generation"].as<std::string>() == generation &&
          config["rink"]["scoreboard"]["perspective_polygon"].IsDefined() &&
          absl::IsAborted(
              hm::stitching::commit_live_stitched_output_rotation(
                  root.string(), rotated_generation, authorize->authorization_id)),
      "a superseded rotation commit must fail without deleting completed-generation scoreboard geometry");

  const auto restore_rollback = restore.ok()
      ? hm::stitching::rollback_live_stitched_output_rotation(
            root.string(), restore->pending_generation, restore->authorization_id)
      : absl::StatusOr<std::optional<hm::stitching::LiveStitchedOutputAuthorization>>(restore.status());
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      restore_rollback.ok() && restore_rollback->has_value() &&
          (*restore_rollback)->authorization_id == authorize->authorization_id &&
          config["rink"]["stitched_output_pending_generation"].as<std::string>() == rotated_generation,
      "rejecting a return to the completed generation must restore its pending predecessor");

  const auto restore_again =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 0.0, "auth-restore-again");
  const auto restore_commit = restore_again.ok()
      ? hm::stitching::commit_live_stitched_output_rotation(
            root.string(), restore_again->pending_generation, restore_again->authorization_id)
      : restore_again.status();
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      restore_commit.ok() && !config["rink"]["stitched_output_pending_generation"].IsDefined(),
      "committing a return to the completed generation must retire pending producer authority");

  const auto reauthorize = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25, "auth-b");
  const auto commit = reauthorize.ok()
      ? hm::stitching::commit_live_stitched_output_rotation(
            root.string(), reauthorize->pending_generation, reauthorize->authorization_id)
      : reauthorize.status();
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      reauthorize.ok() && reauthorize->pending_generation == rotated_generation && commit.ok() &&
          config["rink"]["stitched_output_pending_generation"].as<std::string>() == rotated_generation &&
          !config["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
      "the current exact pending generation must invalidate scoreboard geometry while retaining producer authority");

  const auto return_after_commit =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 0.0, "auth-return-after-commit");
  const auto return_after_commit_status = return_after_commit.ok()
      ? hm::stitching::commit_live_stitched_output_rotation(
            root.string(), return_after_commit->pending_generation, return_after_commit->authorization_id)
      : return_after_commit.status();
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      return_after_commit.ok() && return_after_commit->scoreboard_property_value == "1,2,3,4,5,6,7,8" &&
          return_after_commit_status.ok() &&
          config["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>() ==
              std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}},
      "returning to the completed generation must restore its persisted scoreboard polygon");

  const auto producer_authorization =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25, "auth-producer");
  const auto producer_commit = producer_authorization.ok()
      ? hm::stitching::commit_live_stitched_output_rotation(
            root.string(), producer_authorization->pending_generation, producer_authorization->authorization_id)
      : producer_authorization.status();
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(producer_authorization.ok() && producer_commit.ok(), "producer publication fixture must be authorized");
  config["rink"]["stitched_output_generation"] = rotated_generation;
  config["rink"].remove("stitched_output_pending_generation");
  config["rink"].remove("stitched_output_pending_authorization_id");
  config["rink"].remove("stitched_output_pending_owner_process");
  config["rink"].remove("stitched_output_pending_previous_generation");
  config["rink"].remove("stitched_output_pending_previous_authorization_id");
  config["rink"].remove("stitched_output_pending_previous_owner_process");
  config["rink"].remove("stitched_output_pending_completed_scoreboard_polygon");
  config["rink"]["scoreboard"]["perspective_polygon"] =
      std::vector<std::vector<int>>{{9, 10}, {11, 12}, {13, 14}, {15, 16}};
  std::ofstream(root / "config.yaml") << config << '\n';
  const absl::Status committed_again =
      hm::stitching::commit_live_stitched_output_rotation(root.string(), rotated_generation, "auth-producer");
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      committed_again.ok() && config["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
      "an acknowledgement after exact producer publication must not delete newer scoreboard geometry");

  write_config(root / "config.yaml", generation);
  config = YAML::LoadFile((root / "config.yaml").string());
  config["rink"]["scoreboard"]["perspective_polygon"] = std::vector<std::vector<int>>{{0, 0}, {0, 0}, {0, 0}, {0, 0}};
  std::ofstream(root / "config.yaml") << config << '\n';
  const auto disabled_scoreboard =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 4.0, "auth-disabled");
  const auto disabled_scoreboard_commit = disabled_scoreboard.ok()
      ? hm::stitching::commit_live_stitched_output_rotation(
            root.string(), disabled_scoreboard->pending_generation, disabled_scoreboard->authorization_id)
      : disabled_scoreboard.status();
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      disabled_scoreboard.ok() && !disabled_scoreboard->pending_generation.empty() && disabled_scoreboard_commit.ok() &&
          config["rink"]["scoreboard"]["perspective_polygon"].IsSequence() &&
          config["rink"]["scoreboard"]["perspective_polygon"].size() == 4,
      "live rotation must preserve the scoreboard-disabled sentinel");

  write_config(root / "config.yaml", generation);
  const auto accepted_before_shutdown =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 4.0, "auth-shutdown");
  const auto accepted_before_shutdown_commit = accepted_before_shutdown.ok()
      ? hm::stitching::commit_live_stitched_output_rotation(
            root.string(), accepted_before_shutdown->pending_generation, accepted_before_shutdown->authorization_id)
      : accepted_before_shutdown.status();
  const auto shutdown_rollback = accepted_before_shutdown.ok()
      ? hm::stitching::rollback_live_stitched_output_rotation(
            root.string(), accepted_before_shutdown->pending_generation, accepted_before_shutdown->authorization_id)
      : absl::StatusOr<std::optional<hm::stitching::LiveStitchedOutputAuthorization>>(
            accepted_before_shutdown.status());
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      accepted_before_shutdown.ok() && accepted_before_shutdown_commit.ok() && shutdown_rollback.ok() &&
          !shutdown_rollback->has_value() && !config["rink"]["stitched_output_pending_generation"].IsDefined() &&
          config["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>() ==
              std::vector<std::vector<int>>{{1, 2}, {3, 4}, {5, 6}, {7, 8}},
      "shutdown after runtime acceptance must restore the completed scoreboard polygon");

  write_config(root / "config.yaml", generation);
  const auto older = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25, "auth-older");
  const auto newer = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 4.0, "auth-newer");
  const auto cancel_older = older.ok()
      ? hm::stitching::rollback_live_stitched_output_rotation(
            root.string(), older->pending_generation, older->authorization_id)
      : absl::StatusOr<std::optional<hm::stitching::LiveStitchedOutputAuthorization>>(older.status());
  config = YAML::LoadFile((root / "config.yaml").string());
  const std::string newer_generation = newer.ok() ? newer->pending_generation : std::string{};
  const bool older_cancel_preserved_newer = absl::IsAborted(cancel_older.status()) && !newer_generation.empty() &&
      config["rink"]["stitched_output_pending_generation"].as<std::string>() == newer_generation &&
      config["rink"]["scoreboard"]["perspective_polygon"].IsDefined();
  const auto cancel_newer = newer.ok()
      ? hm::stitching::rollback_live_stitched_output_rotation(root.string(), newer_generation, newer->authorization_id)
      : absl::StatusOr<std::optional<hm::stitching::LiveStitchedOutputAuthorization>>(newer.status());
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      older.ok() && newer.ok() && older_cancel_preserved_newer && cancel_newer.ok() && cancel_newer->has_value() &&
          (*cancel_newer)->pending_generation == older->pending_generation &&
          config["rink"]["stitched_output_pending_generation"].as<std::string>() == older->pending_generation &&
          config["rink"]["scoreboard"]["perspective_polygon"].IsDefined(),
      "exact rollback must preserve a newer authorization and restore its predecessor");

  const auto back_to_older =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25, "auth-older-2");
  const auto delayed_older_rollback = hm::stitching::rollback_live_stitched_output_rotation(
      root.string(), older->pending_generation, older->authorization_id);
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      back_to_older.ok() && absl::IsAborted(delayed_older_rollback.status()) &&
          config["rink"]["stitched_output_pending_authorization_id"].as<std::string>() == "auth-older-2",
      "a delayed B1 rollback must not remove a B2 authorization for the same generation");

  const std::string legacy_generation =
      "hstream-stitched-output-v1\nhugin-bytes:1\nh"
      "post-stitch-rotate-degrees:0\n";
  write_config(root / "config.yaml", legacy_generation);
  const auto legacy = hm::stitching::authorize_live_stitched_output_rotation(root.string(), 1.0, "auth-legacy");
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      absl::IsFailedPrecondition(legacy.status()) &&
          config["rink"]["stitched_output_generation"].as<std::string>() == legacy_generation &&
          !config["rink"]["stitched_output_pending_generation"].IsDefined(),
      "dimensionless generations must not create an inexact live authorization");

  const auto non_finite =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), std::nan(""), "auth-invalid");
  ok &= expect(absl::IsInvalidArgument(non_finite.status()), "non-finite live rotations must be rejected");

  write_config(root / "config.yaml", generation);
  const auto scoreboard_race =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 4.0, "auth-scoreboard-race");
  config = YAML::LoadFile((root / "config.yaml").string());
  const std::vector<std::vector<int>> newer_scoreboard_polygon = {{20, 21}, {22, 23}, {24, 25}, {26, 27}};
  config["rink"]["scoreboard"]["perspective_polygon"] = newer_scoreboard_polygon;
  std::ofstream(root / "config.yaml") << config << '\n';
  const auto scoreboard_race_rollback = scoreboard_race.ok()
      ? hm::stitching::rollback_live_stitched_output_rotation(
            root.string(), scoreboard_race->pending_generation, scoreboard_race->authorization_id)
      : absl::StatusOr<std::optional<hm::stitching::LiveStitchedOutputAuthorization>>(scoreboard_race.status());
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      scoreboard_race_rollback.ok() &&
          config["rink"]["scoreboard"]["perspective_polygon"].as<std::vector<std::vector<int>>>() ==
              newer_scoreboard_polygon,
      "authorization rollback must preserve newer scoreboard geometry");

  write_config(root / "config.yaml", generation);
  std::ofstream(root / "hm_project.pto", std::ios::app) << "new generation\n";
  const auto stale_hugin =
      hm::stitching::authorize_live_stitched_output_rotation(root.string(), 9.25, "auth-stale-hugin");
  config = YAML::LoadFile((root / "config.yaml").string());
  ok &= expect(
      absl::IsAborted(stale_hugin.status()) && !config["rink"]["stitched_output_pending_generation"].IsDefined(),
      "live authorization must reject a persisted generation from replaced Hugin artifacts");

  std::error_code ignored;
  fs::remove_all(root, ignored);
  return ok ? 0 : 1;
}
