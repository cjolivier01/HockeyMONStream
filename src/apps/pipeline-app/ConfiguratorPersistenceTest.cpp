#include "src/apps/pipeline-app/configurator.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <gst/gst.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"

GST_DEBUG_CATEGORY(NVDS_APP);

namespace {

namespace fs = std::filesystem;

bool expect(bool condition, const char* message) {
  if (!condition)
    std::cerr << "FAIL: " << message << '\n';
  return condition;
}

} // namespace

int main() {
  GST_DEBUG_CATEGORY_INIT(NVDS_APP, "NVDS_APP", 0, nullptr);
  bool ok = true;
  const fs::path root = fs::temp_directory_path() / ("configurator-persistence-test-" + std::to_string(::getpid()));
  fs::remove_all(root);
  const fs::path games = root / "games";
  const fs::path game_dir = games / "first-save";
  fs::create_directories(game_dir);
  ::setenv("HM_GAME_DIR", games.c_str(), 1);

  hm::Configurator configurator("first-save", "", hm::Configurator::kUseConfigFileGpu);
  const auto loaded = configurator.load_config();
  ok &= expect(loaded.ok(), "Configurator must load when the private config is initially absent");

  {
    auto lock = hm::stitching::GameConfigTransactionLock::Acquire(game_dir);
    ok &= expect(lock.ok(), "concurrent config creator must acquire the transaction lock");
    if (lock.ok()) {
      YAML::Node concurrent(YAML::NodeType::Map);
      concurrent["hstream_ui"]["keep"] = true;
      ok &= expect(
          hm::stitching::publish_game_config(game_dir, YAML::Dump(concurrent) + "\n").ok(),
          "concurrent config creation must publish");
    }
  }

  YAML::Node desired(YAML::NodeType::Map);
  desired["pipeline"]["generated"] = true;
  ok &= expect(
      configurator.save_private_config(desired).ok(), "Configurator first save after concurrent creation must publish");

  auto final_config = hm::stitching::load_game_config_file(game_dir / "config.yaml");
  ok &= expect(
      final_config.ok() && final_config->has_value() && (**final_config)["pipeline"]["generated"].as<bool>() &&
          (**final_config)["hstream_ui"]["keep"].as<bool>(),
      "Configurator first save must retain keys created after its absent baseline");

  YAML::Node explicit_roles(YAML::NodeType::Map);
  explicit_roles["hstream_ui"]["video_roles"]["left"].push_back(".hstream-ui/left/GX010001.MP4");
  explicit_roles["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX010002.MP4");
  auto selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.ui_roles_are_authoritative && selected.left_is_explicit && selected.right_is_explicit &&
          selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4"} &&
          selected.right == std::vector<std::string>{".hstream-ui/right/GX010002.MP4"},
      "Forced configuration must reconstruct missing runtime video lists from explicit UI roles");

  explicit_roles["game"]["videos"]["left"].push_back("cam2/GX010002.MP4");
  explicit_roles["game"]["videos"]["right"].push_back("cam1/GX010001.MP4");
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4"} &&
          selected.right == std::vector<std::string>{".hstream-ui/right/GX010002.MP4"},
      "Forced configuration must prefer explicit UI roles over mismatched derived runtime lists");

  YAML::Node out_of_order_left(YAML::NodeType::Sequence);
  out_of_order_left.push_back(".hstream-ui/left/GX020001.MP4");
  out_of_order_left.push_back(".hstream-ui/left/GX010001.MP4");
  explicit_roles["hstream_ui"]["video_roles"]["left"] = out_of_order_left;
  explicit_roles["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX020002.MP4");
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.error.empty() &&
          selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"} &&
          selected.right ==
              std::vector<std::string>{".hstream-ui/right/GX010002.MP4", ".hstream-ui/right/GX020002.MP4"},
      "Explicit UI Left/Right roles must be paired and sorted by chapter");

  explicit_roles["hstream_ui"]["video_roles"]["right"][1] = ".hstream-ui/right/GX030002.MP4";
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.error.empty() &&
          selected.left == std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"} &&
          selected.right ==
              std::vector<std::string>{".hstream-ui/right/GX010002.MP4", ".hstream-ui/right/GX030002.MP4"},
      "Explicit camera playlists must not be paired by physical chapter number");

  explicit_roles["hstream_ui"]["video_roles"]["right"].push_back(".hstream-ui/right/GX020002.MP4");
  selected = hm::configurator_internal::select_explicit_stitching_videos(explicit_roles, /*force=*/true);
  ok &= expect(
      selected.error.empty() && selected.left.size() == 2 && selected.right.size() == 3 &&
          selected.right[1] == ".hstream-ui/right/GX020002.MP4" &&
          selected.right[2] == ".hstream-ui/right/GX030002.MP4",
      "Explicit camera playlists with different physical chapter counts must be retained independently");

  YAML::Node saved_uneven_playlists(YAML::NodeType::Map);
  saved_uneven_playlists["game"]["videos"]["left"] = selected.left;
  saved_uneven_playlists["game"]["videos"]["right"] = selected.right;
  const auto saved_selection =
      hm::configurator_internal::select_explicit_stitching_videos(saved_uneven_playlists, /*force=*/false);
  ok &= expect(
      saved_selection.error.empty() && saved_selection.left.size() == 2 && saved_selection.right.size() == 3,
      "A subsequent run must accept independently persisted camera playlists");

  YAML::Node independent_name_schemes(YAML::NodeType::Map);
  independent_name_schemes["hstream_ui"]["video_roles"]["left"] = out_of_order_left;
  independent_name_schemes["hstream_ui"]["video_roles"]["right"].push_back("right-camera.mov");
  independent_name_schemes["hstream_ui"]["video_roles"]["right"].push_back("right-camera-alt.mov");
  const auto independently_named =
      hm::configurator_internal::select_explicit_stitching_videos(independent_name_schemes, /*force=*/true);
  ok &= expect(
      independently_named.error.empty() &&
          independently_named.left ==
              std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"} &&
          independently_named.right == std::vector<std::string>{"right-camera.mov", "right-camera-alt.mov"},
      "Each explicit camera playlist must use its own valid naming and ordering scheme");

  YAML::Node one_sided_out_of_order(YAML::NodeType::Map);
  one_sided_out_of_order["hstream_ui"]["video_roles"]["left"] = out_of_order_left;
  const auto normalized_one_sided =
      hm::configurator_internal::select_explicit_stitching_videos(one_sided_out_of_order, /*force=*/true);
  ok &= expect(
      normalized_one_sided.error.empty() && normalized_one_sided.left_is_explicit &&
          !normalized_one_sided.right_is_explicit &&
          normalized_one_sided.left ==
              std::vector<std::string>{".hstream-ui/left/GX010001.MP4", ".hstream-ui/left/GX020001.MP4"},
      "A one-sided explicit playlist must be normalized before the unambiguous single-file Auto mode uses it");

  YAML::Node restarted_recordings(YAML::NodeType::Map);
  restarted_recordings["hstream_ui"]["video_roles"]["left"].push_back("GX010002.MP4");
  restarted_recordings["hstream_ui"]["video_roles"]["left"].push_back("GX020001.MP4");
  restarted_recordings["hstream_ui"]["video_roles"]["right"].push_back("VID_20260815_101500_001.MP4");
  restarted_recordings["hstream_ui"]["video_roles"]["right"].push_back("VID_20260815_101000_002.MP4");
  const auto normalized_restarts =
      hm::configurator_internal::select_explicit_stitching_videos(restarted_recordings, /*force=*/true);
  ok &= expect(
      normalized_restarts.error.empty() &&
          normalized_restarts.left == std::vector<std::string>{"GX020001.MP4", "GX010002.MP4"} &&
          normalized_restarts.right ==
              std::vector<std::string>{"VID_20260815_101000_002.MP4", "VID_20260815_101500_001.MP4"},
      "Explicit GoPro and Insta360 playlists must sort by recording ID before physical chapter number");

  YAML::Node duplicate_arbitrary(YAML::NodeType::Map);
  duplicate_arbitrary["hstream_ui"]["video_roles"]["left"].push_back("left-camera.mov");
  duplicate_arbitrary["hstream_ui"]["video_roles"]["left"].push_back("left-camera.mov");
  const auto rejected_duplicate =
      hm::configurator_internal::select_explicit_stitching_videos(duplicate_arbitrary, /*force=*/true);
  ok &= expect(
      !rejected_duplicate.error.empty(), "An exact duplicate path must never replay the same arbitrary-named file");

  YAML::Node numbered_parts(YAML::NodeType::Map);
  numbered_parts["hstream_ui"]["video_roles"]["left"].push_back("left-12.mkv");
  numbered_parts["hstream_ui"]["video_roles"]["left"].push_back("left-2.mkv");
  const auto normalized_parts =
      hm::configurator_internal::select_explicit_stitching_videos(numbered_parts, /*force=*/true);
  ok &= expect(
      normalized_parts.error.empty() && normalized_parts.left == std::vector<std::string>{"left-2.mkv", "left-12.mkv"},
      "Explicit left/right part playlists must support Auto formats and sort multi-digit parts numerically");

  YAML::Node heterogeneous_camera(YAML::NodeType::Map);
  heterogeneous_camera["hstream_ui"]["video_roles"]["left"].push_back("VID_20260815_101000_001.MP4");
  heterogeneous_camera["hstream_ui"]["video_roles"]["left"].push_back("GX010007.MP4");
  heterogeneous_camera["hstream_ui"]["video_roles"]["left"].push_back("left-camera.mov");
  const auto preserved_heterogeneous =
      hm::configurator_internal::select_explicit_stitching_videos(heterogeneous_camera, /*force=*/true);
  ok &= expect(
      preserved_heterogeneous.error.empty() &&
          preserved_heterogeneous.left ==
              std::vector<std::string>{"VID_20260815_101000_001.MP4", "GX010007.MP4", "left-camera.mov"},
      "A heterogeneous explicit camera playlist must preserve the user's total recording order");

  YAML::Node duplicate_persisted(YAML::NodeType::Map);
  duplicate_persisted["game"]["videos"]["left"].push_back("cam1/GX010001.MP4");
  duplicate_persisted["game"]["videos"]["left"].push_back("cam1/GX010001.MP4");
  duplicate_persisted["game"]["videos"]["right"].push_back("cam2/GX010002.MP4");
  const auto rejected_persisted =
      hm::configurator_internal::select_explicit_stitching_videos(duplicate_persisted, /*force=*/false);
  ok &= expect(
      !rejected_persisted.error.empty(), "A persisted camera playlist must never replay an exact duplicate path");

  ok &= expect(
      !hm::configurator_internal::validate_mixed_explicit_auto_playlists(
           /*left_is_explicit=*/true,
           /*right_is_explicit=*/false,
           /*auto_left_chapters=*/0,
           /*auto_right_chapters=*/3)
              .ok() &&
          !hm::configurator_internal::validate_mixed_explicit_auto_playlists(
               /*left_is_explicit=*/false,
               /*right_is_explicit=*/true,
               /*auto_left_chapters=*/2,
               /*auto_right_chapters=*/0)
               .ok(),
      "Mixed Explicit/Auto selection must reject an ambiguous multi-file Auto camera timeline");
  ok &= expect(
      hm::configurator_internal::validate_mixed_explicit_auto_playlists(
          /*left_is_explicit=*/true,
          /*right_is_explicit=*/false,
          /*auto_left_chapters=*/0,
          /*auto_right_chapters=*/1)
              .ok() &&
          hm::configurator_internal::validate_mixed_explicit_auto_playlists(
              /*left_is_explicit=*/true,
              /*right_is_explicit=*/true,
              /*auto_left_chapters=*/4,
              /*auto_right_chapters=*/6)
              .ok(),
      "A single-file Auto side and two independently explicit playlists are unambiguous");

  const fs::path superseded_force_dir = games / "superseded-force";
  fs::create_directories(superseded_force_dir);
  std::ofstream(superseded_force_dir / "seam_file.png") << "newer generation\n";
  YAML::Node stale_force_config(YAML::NodeType::Map);
  stale_force_config["pipeline"]["application"]["complete-configuration"] = "1";
  stale_force_config["pipeline"]["hmstitcher"]["enable"] = "1";
  stale_force_config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  stale_force_config["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
  stale_force_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = true;
  stale_force_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "stale-force";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_force_dir, YAML::Dump(stale_force_config) + "\n").ok(),
      "stale forced configuration fixture must publish");
  hm::Configurator forced_configurator("superseded-force", "", hm::Configurator::kUseConfigFileGpu);
  ok &= expect(forced_configurator.configure().ok(), "forced configurator must load its stale snapshot");
  auto initially_current =
      hm::stitching::is_stitching_invalidation_cleanup_applied(superseded_force_dir.string(), "stale-force");
  ok &= expect(
      initially_current.ok() && initially_current.value(),
      "forced invalidation must initially pass the pre-clean revalidation");
  YAML::Node stale_offset_save = YAML::Clone(stale_force_config);
  stale_offset_save["game"]["stitching"]["frame_offsets"]["left"] = "9";
  stale_offset_save["game"]["stitching"]["frame_offsets"]["right"] = "0";
  stale_force_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
  stale_force_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "newer-force";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_force_dir, YAML::Dump(stale_force_config) + "\n").ok(),
      "newer forced invalidation fixture must publish");
  const absl::Status stale_save_status =
      forced_configurator.save_private_config(stale_offset_save, /*expected_invalidation_id=*/"stale-force");
  auto after_stale_save = hm::stitching::load_game_config_file(superseded_force_dir / "config.yaml");
  ok &= expect(
      stale_save_status.code() == absl::StatusCode::kAborted && after_stale_save.ok() &&
          after_stale_save->has_value() && !(**after_stale_save)["game"]["stitching"]["frame_offsets"] &&
          (**after_stale_save)["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>() ==
              "newer-force",
      "A superseded forced save must not republish stale frame offsets after initial revalidation");
  const absl::Status non_forced_status = forced_configurator.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"stale-force");
  ok &= expect(
      non_forced_status.code() == absl::StatusCode::kAborted && fs::exists(superseded_force_dir / "seam_file.png"),
      "A non-forced input-stale run must abort before using a superseding artifact generation");
  const absl::Status forced_status = forced_configurator.complete_configuration(
      /*force=*/true,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"stale-force");
  ok &= expect(
      forced_status.code() == absl::StatusCode::kAborted && fs::exists(superseded_force_dir / "seam_file.png"),
      "Forced configuration must abort before using or deleting a superseding artifact generation");

  const fs::path superseded_complete_dir = games / "superseded-complete";
  fs::create_directories(superseded_complete_dir);
  YAML::Node reserved_complete_config(YAML::NodeType::Map);
  reserved_complete_config["pipeline"]["application"]["complete-configuration"] = "1";
  reserved_complete_config["pipeline"]["hmstitcher"]["enable"] = "1";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["status"] = "complete";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "reserved-complete-a";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_complete_dir, YAML::Dump(reserved_complete_config) + "\n").ok(),
      "reserved complete owner fixture must publish");
  hm::Configurator complete_configurator("superseded-complete", "", hm::Configurator::kUseConfigFileGpu);
  ok &= expect(complete_configurator.configure().ok(), "reserved complete configurator must load its owner snapshot");
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["status"] = "pending";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["stale_from"] = "input";
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["artifacts_invalidated"] = false;
  reserved_complete_config["hstream_ui"]["stitching_calibration"]["invalidation_id"] = "reserved-complete-b";
  ok &= expect(
      hm::stitching::publish_game_config(superseded_complete_dir, YAML::Dump(reserved_complete_config) + "\n").ok(),
      "newer owner must supersede a reserved complete run after load_config");
  const absl::Status superseded_complete_status = complete_configurator.complete_configuration(
      /*force=*/false,
      /*clean_stitching_artifacts=*/false,
      /*clean_stitching_from_control_points=*/false,
      /*clean_expected_invalidation_id=*/"reserved-complete-a");
  ok &= expect(
      superseded_complete_status.code() == absl::StatusCode::kAborted,
      "A reserved complete owner superseded after load_config must abort at the final launch boundary");

  ::unsetenv("HM_GAME_DIR");
  fs::remove_all(root);
  return ok ? 0 : 1;
}
