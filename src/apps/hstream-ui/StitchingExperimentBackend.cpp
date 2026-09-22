#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/PlayerFrameInputStore.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <regex>
#include <set>
#include <system_error>
#include <vector>

#include "yaml-cpp/yaml.h"

namespace fs = std::filesystem;

namespace {

const std::set<std::string> kVideoExtensions = {".avi", ".m4v", ".mkv", ".mov", ".mp4"};
const std::regex kCalibrationAsset(R"(^(?:.*[-_])?calibration(?:[-_].*)?\.(?:json|ya?ml)$)", std::regex::icase);
const std::regex kRootCameraVideo(
    R"((?:G[A-Z][0-9]{6}|VID_[0-9]{8}_[0-9]{6}_[0-9]{3}|(?:left|right)(?:-[0-9]+)?)\.(?:avi|m4v|mkv|mov|mp4))",
    std::regex::icase);

bool is_video(const fs::path& path) {
  std::string extension = path.extension().string();
  std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return kVideoExtensions.count(extension) != 0;
}

absl::StatusOr<fs::path> normalized_source(
    const fs::path& source_game_directory,
    const fs::path& configured,
    const char* description = "video") {
  if (configured.empty())
    return absl::InvalidArgumentError("Configured experiment input path must not be empty");
  fs::path source = configured;
  if (source.is_relative())
    source = source_game_directory / source;
  std::error_code error;
  const fs::path canonical_source = fs::canonical(source, error);
  if (error || !fs::is_regular_file(canonical_source))
    return absl::NotFoundError("Experiment input " + std::string(description) + " is missing: " + source.string());
  const fs::path canonical_game = fs::canonical(source_game_directory, error);
  if (error)
    return absl::NotFoundError("Experiment game directory is unavailable");
  fs::path relative = canonical_source.lexically_relative(canonical_game);
  if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..")
    return absl::InvalidArgumentError(
        "Experiment input " + std::string(description) + " must be inside the selected game: " + configured.string());
  return relative;
}

absl::Status link_input(const fs::path& canonical_source, const fs::path& destination) {
  std::error_code error;
  fs::create_directories(destination.parent_path(), error);
  if (error)
    return absl::InternalError("Unable to create experiment input directory: " + error.message());
  if (fs::exists(destination, error) || fs::is_symlink(destination, error))
    return absl::AlreadyExistsError("Duplicate experiment input destination: " + destination.string());
  fs::create_symlink(canonical_source, destination, error);
  return error ? absl::InternalError("Unable to link experiment input: " + error.message()) : absl::OkStatus();
}

absl::Status link_configured_videos(
    YAML::Node config,
    const fs::path& source_game_directory,
    const fs::path& candidate_game_directory,
    std::set<fs::path>* linked) {
  for (const std::pair<const char*, const char*> section :
       {std::make_pair("game", "videos"), std::make_pair("hstream_ui", "video_roles")}) {
    YAML::Node roles = config[section.first][section.second];
    if (!roles || !roles.IsMap())
      continue;
    for (const char* role : {"left", "center", "right"}) {
      YAML::Node values = roles[role];
      if (!values || !values.IsSequence())
        continue;
      for (size_t index = 0; index < values.size(); ++index) {
        if (!values[index].IsScalar())
          return absl::InvalidArgumentError("Experiment video roles must contain path strings");
        fs::path relative;
        HM_ASSIGN_OR_RETURN(relative, normalized_source(source_game_directory, values[index].as<std::string>()));
        if (!is_video(relative))
          return absl::InvalidArgumentError(
              "Configured experiment video is not a supported video file: " + relative.string());
        if (linked->insert(relative).second)
          HM_RETURN_IF_ERROR(link_input(source_game_directory / relative, candidate_game_directory / relative));
        values[index] = relative.generic_string();
      }
    }
  }
  return absl::OkStatus();
}

absl::Status link_auto_videos(
    const fs::path& source_game_directory,
    const fs::path& candidate_game_directory,
    std::set<fs::path>* linked) {
  const std::regex camera_directory(R"(^cam[0-9]+$)", std::regex::icase);
  std::vector<fs::path> directories{source_game_directory};
  std::error_code error;
  for (const auto& entry : fs::directory_iterator(source_game_directory, error)) {
    if (error)
      return absl::InternalError("Unable to inspect experiment game inputs: " + error.message());
    if (entry.is_directory() && !entry.is_symlink() &&
        std::regex_match(entry.path().filename().string(), camera_directory)) {
      directories.push_back(entry.path());
    }
  }
  for (const fs::path& directory : directories) {
    for (const auto& entry : fs::directory_iterator(directory, error)) {
      if (error)
        return absl::InternalError("Unable to inspect experiment camera inputs: " + error.message());
      if (!entry.is_regular_file() || !is_video(entry.path()) ||
          (directory == source_game_directory &&
           !std::regex_match(entry.path().filename().string(), kRootCameraVideo))) {
        continue;
      }
      fs::path relative;
      HM_ASSIGN_OR_RETURN(relative, normalized_source(source_game_directory, entry.path(), "video"));
      if (!is_video(relative))
        return absl::InvalidArgumentError(
            "Auto-discovered experiment video resolves to a non-video file: " + entry.path().string());
      if (linked->insert(relative).second)
        HM_RETURN_IF_ERROR(link_input(source_game_directory / relative, candidate_game_directory / relative));
    }
  }
  return absl::OkStatus();
}

absl::Status link_calibration_assets(
    const fs::path& source_game_directory,
    const fs::path& candidate_game_directory,
    const std::set<fs::path>& linked_videos) {
  std::set<fs::path> directories{fs::path{}};
  for (const fs::path& video : linked_videos)
    directories.insert(video.parent_path());
  std::set<fs::path> linked_assets;
  std::error_code error;
  for (const fs::path& relative_directory : directories) {
    const fs::path source_directory = source_game_directory / relative_directory;
    for (const auto& entry : fs::directory_iterator(source_directory, error)) {
      if (error)
        return absl::InternalError("Unable to inspect camera calibration assets: " + error.message());
      if (!entry.is_regular_file() || !std::regex_match(entry.path().filename().string(), kCalibrationAsset)) {
        continue;
      }
      fs::path relative;
      HM_ASSIGN_OR_RETURN(relative, normalized_source(source_game_directory, entry.path(), "calibration asset"));
      if (!std::regex_match(relative.filename().string(), kCalibrationAsset))
        return absl::InvalidArgumentError(
            "Experiment calibration asset resolves to a reserved file: " + entry.path().string());
      if (linked_assets.insert(relative).second)
        HM_RETURN_IF_ERROR(link_input(source_game_directory / relative, candidate_game_directory / relative));
    }
  }
  return absl::OkStatus();
}

void promote_generated_video_roles(YAML::Node config) {
  YAML::Node ui_roles = config["hstream_ui"]["video_roles"];
  const YAML::Node videos = config["game"]["videos"];
  const bool ui_left = ui_roles["left"] && ui_roles["left"].IsSequence() && ui_roles["left"].size() > 0;
  const bool ui_right = ui_roles["right"] && ui_roles["right"].IsSequence() && ui_roles["right"].size() > 0;
  if (!ui_left && !ui_right && videos && videos.IsMap()) {
    if (videos["left"])
      ui_roles["left"] = YAML::Clone(videos["left"]);
    if (videos["right"])
      ui_roles["right"] = YAML::Clone(videos["right"]);
  }
}

void remove_downstream_generation(YAML::Node config) {
  YAML::Node stitching = config["stitching"];
  if (stitching && stitching.IsMap()) {
    stitching.remove("control_points");
    stitching.remove("generated_field_mask_post_stitch_rotate_degrees");
  }
  YAML::Node game_stitching = config["game"]["stitching"];
  if (game_stitching && game_stitching.IsMap())
    game_stitching.remove("control_points");
  YAML::Node rink = config["rink"];
  if (rink && rink.IsMap()) {
    for (const char* key :
         {"stitched_output_generation",
          "stitched_output_persisted_rotation_degrees",
          "stitched_output_pending_generation",
          "stitched_output_pending_authorization_id",
          "stitched_output_pending_owner_process",
          "stitched_output_pending_previous_generation",
          "stitched_output_pending_previous_authorization_id",
          "stitched_output_pending_previous_owner_process",
          "stitched_output_pending_completed_scoreboard_polygon",
          "ice_contours_mask_count",
          "ice_contours_mask_centroid",
          "ice_contours_combined_bbox"}) {
      rink.remove(key);
    }
    YAML::Node scoreboard = rink["scoreboard"];
    if (scoreboard && scoreboard.IsMap())
      scoreboard.remove("perspective_polygon");
  }
}

absl::StatusOr<std::string> reusable_selection_fingerprint(
    const YAML::Node& config,
    const StitchingExperimentSettings& settings) {
  std::string fingerprint;
  HM_ASSIGN_OR_RETURN(fingerprint, hm::stitching::player_frame_selection_fingerprint(config));
  if (fingerprint.empty())
    return fingerprint;
  const YAML::Node selection = config["stitching"]["calibration_frame_selection"];
  if (selection["selected"].size() != static_cast<size_t>(settings.frame_count))
    return std::string();
  const YAML::Node inputs = config["stitching"]["calibration_frame_inputs_fingerprint"];
  if (inputs && (!inputs.IsScalar() || inputs.as<std::string>() != fingerprint))
    return absl::FailedPreconditionError("The saved frame input cache reference does not match its selection");
  const std::string anchor = selection["context"]["decode_anchor_ns"].as<std::string>();
  uint64_t anchor_ns = 0;
  const auto parsed = std::from_chars(anchor.data(), anchor.data() + anchor.size(), anchor_ns);
  if (parsed.ec != std::errc() || parsed.ptr != anchor.data() + anchor.size())
    return absl::InvalidArgumentError("The saved player frames have an invalid decode anchor");
  try {
    if (anchor_ns != hm::stitch_frame_time_to_nanoseconds(settings.stitch_frame_time))
      return absl::FailedPreconditionError(
          "The saved player frames fix the reference time. Restore that time or explicitly change the frame count; "
          "the saved selection was preserved.");
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Invalid candidate reference time: " + std::string(error.what()));
  }
  return fingerprint;
}

absl::Status configure_candidate(
    YAML::Node config,
    const StitchingExperimentSettings& settings,
    const std::string& invalidation_id) {
  std::string selection;
  HM_ASSIGN_OR_RETURN(selection, reusable_selection_fingerprint(config, settings));
  config["stitching"]["calibration_frame_count"] = settings.frame_count;
  if (selection.empty()) {
    config["stitching"]["stitch_frame_time"] = settings.stitch_frame_time;
    config["stitching"].remove("calibration_frame_selection");
    config["stitching"].remove("calibration_frame_inputs_fingerprint");
  } else {
    // The exact persisted timestamp spelling (including absence) belongs to
    // the plan's source context. Solve settings do not replace these inputs.
    HM_RETURN_IF_ERROR(hm::stitching::validate_player_frame_selection_sources(config));
  }
  if (settings.rink_rotation_degrees.has_value()) {
    auto framing = hm::stitching::read_stitch_projection_framing(config);
    if (!framing.ok())
      return framing.status();
    framing->rotation_degrees[1] = (*settings.rink_rotation_degrees)[1];
    framing->rotation_degrees[2] = (*settings.rink_rotation_degrees)[2];
    framing->rotation_inherited = false;
    hm::stitching::write_stitch_projection_framing(config, *framing);
  }
  YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
  calibration["control_points"] = settings.control_points;
  calibration["frame_count"] = settings.frame_count;
  calibration["status"] = "pending";
  calibration["rink_mask_status"] = "pending";
  calibration["stale_from"] = "input";
  calibration["artifacts_invalidated"] = false;
  calibration["invalidation_id"] = invalidation_id;
  remove_downstream_generation(config);

  // The CLI reserves the immutable backend generation only after baseline,
  // user, game, and command-line layers have resolved to one effective tuple.
  // A copied game config alone cannot safely pre-reserve inherited choices.
  calibration.remove("backend_generation");
  return absl::OkStatus();
}

absl::Status write_config(const fs::path& path, const YAML::Node& config) {
  std::error_code error;
  const fs::file_status destination = fs::symlink_status(path, error);
  if (error && error != std::errc::no_such_file_or_directory)
    return absl::InternalError("Unable to inspect experiment config destination: " + error.message());
  if (!error && destination.type() != fs::file_type::not_found)
    return absl::AlreadyExistsError("Experiment config destination already exists: " + path.string());
  std::ofstream output(path, std::ios::out | std::ios::trunc);
  if (!output)
    return absl::InternalError("Unable to create experiment config: " + path.string());
  output << YAML::Dump(config) << '\n';
  output.flush();
  return output ? absl::OkStatus() : absl::InternalError("Unable to write experiment config: " + path.string());
}

void copy_node(YAML::Node destination, const YAML::Node& source, const char* key) {
  if (source && source.IsMap() && source[key] && source[key].IsDefined())
    destination[key] = YAML::Clone(source[key]);
  else if (destination && destination.IsMap())
    destination.remove(key);
}

absl::Status reconcile_selected_video_paths(
    YAML::Node current,
    const YAML::Node& selected,
    const fs::path& game_directory) {
  const YAML::Node selection = selected["stitching"]["calibration_frame_selection"];
  if (!selection || selection.IsNull())
    return absl::OkStatus();
  std::error_code error;
  const fs::path directory = fs::absolute(game_directory, error).lexically_normal();
  if (error)
    return absl::InvalidArgumentError("Unable to resolve the selected game's directory: " + error.message());
  for (const char* role : {"left", "right"}) {
    const YAML::Node selected_paths = selected["game"]["videos"][role];
    if (!selected_paths.IsSequence() || selected_paths.size() == 0)
      return absl::InvalidArgumentError("Selected player frames require a resolved camera playlist");
    std::vector<fs::path> relative_paths;
    for (const auto& value : selected_paths) {
      if (!value.IsScalar())
        return absl::InvalidArgumentError("Selected camera chapters must be paths");
      const fs::path relative = fs::path(value.as<std::string>()).lexically_normal();
      if (relative.empty() || relative == "." || relative.is_absolute() || *relative.begin() == "..")
        return absl::InvalidArgumentError("Selected camera chapters must remain inside the game");
      relative_paths.push_back(relative);
    }
    for (const auto& section : {std::make_pair("game", "videos"), std::make_pair("hstream_ui", "video_roles")}) {
      const YAML::Node values = current;
      const YAML::Node root = values[section.first];
      if (!root || !root.IsMap())
        continue;
      const YAML::Node roles = root[section.second];
      if (!roles || !roles.IsMap())
        continue;
      YAML::Node paths = roles[role];
      if (!paths || paths.IsNull() || (paths.IsSequence() && paths.size() == 0))
        continue;
      if (!paths.IsSequence() || paths.size() != relative_paths.size())
        return absl::AbortedError("The selected game's camera playlist differs from the player-frame candidate");
      const bool preserve_order = std::string(section.first) == "game";
      std::set<size_t> matched;
      for (size_t index = 0; index < paths.size(); ++index) {
        if (!paths[index].IsScalar())
          return absl::InvalidArgumentError("The selected game's camera chapters must be paths");
        const fs::path path = (directory / paths[index].as<std::string>()).lexically_normal();
        std::optional<size_t> match;
        for (size_t candidate = 0; candidate < relative_paths.size(); ++candidate) {
          // Preserve resolved playlist order. UI roles may precede the runner's
          // chapter sorting, so only normalize their spellings, never their order.
          if (matched.count(candidate) || (preserve_order && candidate != index))
            continue;
          const fs::path expected = directory / relative_paths[candidate];
          // Identical paths need no media access: existing artifacts can still
          // be promoted while their original recordings are unavailable.
          error.clear();
          if (path == expected || (fs::equivalent(path, expected, error) && !error)) {
            match = candidate;
            break;
          }
        }
        if (!match)
          return absl::AbortedError("The selected game's camera input differs from the player-frame candidate");
        matched.insert(*match);
        paths[index] = relative_paths[*match].generic_string();
      }
    }
  }
  return absl::OkStatus();
}

absl::Status copy_selected_frame_inputs(
    YAML::Node config,
    const YAML::Node& source_config,
    const fs::path& source_directory,
    const fs::path& candidate_directory,
    bool freeze_camera) {
  YAML::Node localized = YAML::Clone(source_config);
  YAML::Node videos = localized["game"]["videos"];
  for (const char* role : {"left", "right"}) {
    YAML::Node paths = videos[role];
    if (!paths.IsSequence() || paths.size() == 0)
      return absl::InvalidArgumentError("The selected frames have no resolved camera playlist");
    for (size_t index = 0; index < paths.size(); ++index) {
      const fs::path path = paths[index].as<std::string>();
      const fs::path relative = path.is_absolute() ? path.lexically_relative(source_directory) : path;
      if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
        return absl::InvalidArgumentError("A selected camera path cannot be localized to the candidate");
      std::error_code error;
      if (!fs::equivalent(source_directory / relative, candidate_directory / relative, error) || error)
        return absl::AbortedError("Candidate media differ from the saved frame selection");
      paths[index] = relative.generic_string();
    }
  }
  // Existing explicit roles/order and synchronization must agree; only missing
  // resolved fields may be filled from the selection owner.
  HM_RETURN_IF_ERROR(reconcile_selected_video_paths(config, localized, candidate_directory));
  const auto offsets_for = [](const YAML::Node& values) {
    const YAML::Node game = values["game"];
    const YAML::Node game_stitching = game && game.IsMap() ? game["stitching"] : YAML::Node();
    const YAML::Node game_offsets =
        game_stitching && game_stitching.IsMap() ? game_stitching["frame_offsets"] : YAML::Node();
    if (game_offsets && !game_offsets.IsNull())
      return game_offsets;
    const YAML::Node stitching = values["stitching"];
    return stitching && stitching.IsMap() ? stitching["frame_offsets"] : YAML::Node();
  };
  const YAML::Node source_offsets = offsets_for(source_config);
  const YAML::Node current_offsets = offsets_for(config);
  for (const char* role : {"left", "right"}) {
    if (current_offsets && current_offsets[role] &&
        current_offsets[role].as<double>() != source_offsets[role].as<double>())
      return absl::AbortedError("Candidate synchronization differs from the saved frame selection");
    config["hstream_ui"]["video_roles"][role] = YAML::Clone(videos[role]);
  }
  config["game"]["videos"] = YAML::Clone(videos);
  config["game"]["stitching"]["frame_offsets"] = YAML::Clone(source_offsets);
  config["stitching"].remove("frame_offsets");
  if (freeze_camera) {
    for (const char* key : {"camera_configs", "camera_config", "camera_fov"})
      copy_node(config["stitching"], source_config["stitching"], key);
  }
  copy_node(config["stitching"], source_config["stitching"], "stitch_frame_time");
  copy_node(config["stitching"], source_config["stitching"], "calibration_frame_selection");
  copy_node(config["stitching"], source_config["stitching"], "calibration_frame_inputs_fingerprint");
  return hm::stitching::validate_player_frame_selection_sources(config);
}

absl::Status copy_selected_input_bundle(
    const fs::path& source_directory,
    const fs::path& candidate_directory,
    YAML::Node config) {
  const YAML::Node values = config;
  const YAML::Node stitching = values["stitching"];
  const YAML::Node selection = stitching && stitching.IsMap() ? stitching["calibration_frame_selection"] : YAML::Node();
  if (!selection || selection.IsNull())
    return absl::OkStatus();
  hm::stitching::PlayerFrameSelectionPlan plan;
  HM_ASSIGN_OR_RETURN(plan, hm::stitching::ParsePlayerFrameSelectionPlan(selection));
  const auto copied = hm::stitching::CopyPlayerFrameInputs(source_directory, candidate_directory, plan);
  const YAML::Node marker = stitching["calibration_frame_inputs_fingerprint"];
  if (marker && (!marker.IsScalar() || marker.as<std::string>() != plan.fingerprint))
    return absl::FailedPreconditionError("The saved frame input cache reference does not match its selection");
  if (absl::IsNotFound(copied) && (!marker || marker.IsNull()))
    return absl::OkStatus();
  HM_RETURN_IF_ERROR(copied);
  config["stitching"]["calibration_frame_inputs_fingerprint"] = plan.fingerprint;
  return absl::OkStatus();
}

absl::StatusOr<std::string> read_inspection_file(const fs::path& path, size_t limit) {
  std::error_code error;
  const auto status = fs::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory || status.type() == fs::file_type::not_found)
    return absl::NotFoundError("Calibration inspection file is missing: " + path.string());
  if (error || status.type() != fs::file_type::regular)
    return absl::FailedPreconditionError("Calibration inspection file must be a regular owned file");
  std::ifstream input(path, std::ios::binary);
  if (!input)
    return absl::InternalError("Unable to read calibration inspection file");
  std::string bytes(limit + 1, '\0');
  input.read(bytes.data(), bytes.size());
  bytes.resize(input.gcount());
  if (input.bad() || bytes.size() > limit)
    return absl::ResourceExhaustedError("Calibration inspection file exceeds its byte limit");
  return bytes;
}

// Called only inside Hugin's promotion callback, with source/destination
// artifact locks and destination config ownership already held.
absl::Status copy_ordinary_inspection_for_promotion(
    const fs::path& source_game,
    const fs::path& destination_game,
    const YAML::Node& selected_config) {
  const YAML::Node selection = selected_config["stitching"]["calibration_frame_selection"];
  if (selection && !selection.IsNull())
    return absl::OkStatus();
  const YAML::Node calibration = selected_config["hstream_ui"]["stitching_calibration"];
  const std::string owner = calibration["invalidation_id"].as<std::string>("");
  if (!std::regex_match(owner, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,159}")))
    return absl::InvalidArgumentError("Invalid calibration inspection owner during promotion");
  const fs::path source = source_game / "calibration-frame-inspection" / owner;
  for (const auto& directory : {source.parent_path(), source}) {
    std::error_code error;
    const auto type = fs::symlink_status(directory, error).type();
    if (error == std::errc::no_such_file_or_directory || type == fs::file_type::not_found)
      return absl::OkStatus();
    if (error || type != fs::file_type::directory)
      return absl::FailedPreconditionError("Calibration inspection must remain in owned directories");
  }
  std::string manifest_bytes;
  auto manifest_file = read_inspection_file(source / "frames.yaml", 64 * 1024);
  if (absl::IsNotFound(manifest_file.status())) {
    std::error_code error;
    if (!fs::exists(source, error) && !error)
      return absl::OkStatus(); // Legacy candidate without inspection artifacts.
  }
  HM_ASSIGN_OR_RETURN(manifest_bytes, std::move(manifest_file));
  const YAML::Node manifest = YAML::Load(manifest_bytes);
  const size_t count = manifest["expected_pair_count"].as<size_t>(0);
  if (manifest["version"].as<int>(0) != 1 || manifest["invalidation_id"].as<std::string>("") != owner || count == 0 ||
      count > 16 || count != calibration["frame_count"].as<size_t>(0) || !manifest["pairs"].IsSequence() ||
      manifest["pairs"].size() != count)
    return absl::FailedPreconditionError("Cannot promote incomplete or mismatched calibration frame inspection");
  std::vector<std::pair<std::string, std::string>> files;
  files.emplace_back("frames.yaml", std::move(manifest_bytes));
  for (size_t index = 0; index < count; ++index) {
    if (manifest["pairs"][index]["index"].as<size_t>(16) != index)
      return absl::FailedPreconditionError("Invalid calibration frame inspection ordering");
    for (const char* camera : {"left", "right"}) {
      const std::string filename = std::string(camera) + "_" + std::to_string(index) + ".jpg";
      std::string contents;
      HM_ASSIGN_OR_RETURN(contents, read_inspection_file(source / filename, 4 * 1024 * 1024));
      files.emplace_back(filename, std::move(contents));
    }
  }
  const fs::path root = destination_game / "calibration-frame-inspection";
  const fs::path destination = root / owner;
  std::error_code error;
  const auto root_type = fs::symlink_status(root, error).type();
  if (error && error != std::errc::no_such_file_or_directory)
    return absl::InternalError("Unable to inspect the destination calibration inspection directory");
  if (!error && root_type != fs::file_type::not_found && root_type != fs::file_type::directory)
    return absl::FailedPreconditionError("Destination calibration inspection directory must not be a symlink");
  error.clear();
  if (fs::exists(destination, error)) {
    if (fs::symlink_status(destination, error).type() != fs::file_type::directory || error)
      return absl::FailedPreconditionError("Destination calibration inspection owner is not a private directory");
    for (const auto& [name, contents] : files) {
      std::string existing;
      HM_ASSIGN_OR_RETURN(existing, read_inspection_file(destination / name, 4 * 1024 * 1024));
      if (existing != contents)
        return absl::FailedPreconditionError("A different calibration inspection already owns the destination ID");
    }
    return absl::OkStatus();
  }
  fs::create_directories(root, error);
  if (error)
    return absl::InternalError("Cannot create promoted calibration inspection directory: " + error.message());
  std::string pattern = (root / ".staging-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const char* created = ::mkdtemp(writable.data());
  if (!created)
    return absl::InternalError("Cannot stage promoted calibration inspection");
  const fs::path staging(created);
  struct Cleanup {
    fs::path path;
    ~Cleanup() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  } cleanup{staging};
  for (const auto& [name, contents] : files) {
    std::ofstream output(staging / name, std::ios::binary);
    output.write(contents.data(), contents.size());
    output.close();
    if (!output)
      return absl::InternalError("Cannot write promoted calibration inspection");
    HM_RETURN_IF_ERROR(hm::stitching::fsync_stitch_path(staging / name));
  }
  HM_RETURN_IF_ERROR(hm::stitching::fsync_stitch_path(staging, true));
  fs::rename(staging, destination, error);
  if (error)
    return absl::InternalError("Cannot publish promoted calibration inspection: " + error.message());
  return hm::stitching::fsync_stitch_path(root, true);
}

} // namespace

absl::StatusOr<std::string> ReusableStitchingExperimentSelectionFingerprint(
    const fs::path& game_directory,
    const StitchingExperimentSettings& settings) {
  auto lock = hm::stitching::GameConfigTransactionLock::Acquire(game_directory);
  if (!lock.ok())
    return lock.status();
  try {
    return reusable_selection_fingerprint(YAML::LoadFile((game_directory / "config.yaml").string()), settings);
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Unable to read saved frame selection: " + std::string(error.what()));
  }
}

absl::StatusOr<std::optional<StitchingExperimentWorkspace>> MainStitchingExperimentWorkspace(
    const fs::path& game_directory,
    const StitchingExperimentSettings& displayed_settings) {
  auto lock = hm::stitching::GameConfigTransactionLock::Acquire(game_directory);
  if (!lock.ok())
    return lock.status();
  try {
    YAML::Node config = YAML::LoadFile((game_directory / "config.yaml").string());
    const YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    std::string fingerprint;
    HM_ASSIGN_OR_RETURN(fingerprint, hm::stitching::player_frame_selection_fingerprint(config));
    if (fingerprint.empty() && calibration["status"].as<std::string>("") != "complete")
      return std::nullopt;
    StitchingExperimentSettings settings = displayed_settings;
    settings.control_points = calibration["control_points"].as<int>(settings.control_points);
    settings.frame_count = fingerprint.empty()
        ? calibration["frame_count"].as<int>(settings.frame_count)
        : static_cast<int>(config["stitching"]["calibration_frame_selection"]["selected"].size());
    settings.stitch_frame_time = config["stitching"]["stitch_frame_time"].as<std::string>(settings.stitch_frame_time);
    if (!fingerprint.empty()) {
      const uint64_t anchor =
          config["stitching"]["calibration_frame_selection"]["context"]["decode_anchor_ns"].as<uint64_t>();
      if (anchor >= 24ULL * 3600ULL * 1000000000ULL || anchor % 1000000ULL != 0)
        return absl::InvalidArgumentError("The main calibration frame selection has an invalid reference time");
      char timestamp[13];
      const uint64_t seconds = anchor / 1000000000ULL;
      std::snprintf(
          timestamp,
          sizeof(timestamp),
          "%02u:%02u:%02u.%03u",
          static_cast<unsigned>(seconds / 3600),
          static_cast<unsigned>((seconds / 60) % 60),
          static_cast<unsigned>(seconds % 60),
          static_cast<unsigned>((anchor / 1000000ULL) % 1000));
      settings.stitch_frame_time = timestamp;
    }
    return StitchingExperimentWorkspace{
        .root = game_directory.parent_path(),
        .game_directory = game_directory,
        .game_id = game_directory.filename().string(),
        .invalidation_id = calibration["invalidation_id"].as<std::string>(""),
        .settings = settings};
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Unable to inspect the main calibration: " + std::string(error.what()));
  }
}

absl::StatusOr<StitchingExperimentWorkspace> CreateStitchingExperimentWorkspace(
    const fs::path& source_game_directory,
    const fs::path& experiment_root,
    const StitchingExperimentSettings& settings,
    int sequence) {
  if (settings.control_points <= 0 || settings.frame_count <= 0 || sequence <= 0)
    return absl::InvalidArgumentError("Stitching experiment counts and sequence must be positive");
  try {
    (void)hm::stitch_frame_time_to_nanoseconds(settings.stitch_frame_time);
  } catch (const std::exception& exception) {
    return absl::InvalidArgumentError(std::string("Invalid stitching experiment start frame: ") + exception.what());
  }
  std::error_code error;
  if (!fs::is_directory(source_game_directory, error) || error)
    return absl::NotFoundError("Select an existing game directory for stitching experiments");
  const fs::path source_config = source_game_directory / "config.yaml";
  if (!fs::is_regular_file(source_config, error) || error)
    return absl::NotFoundError("The selected game has no config.yaml");
  fs::create_directories(experiment_root, error);
  if (error)
    return absl::InternalError("Unable to create stitching experiment root: " + error.message());

  const std::string game_id = source_game_directory.filename().string() + "-stitch-exp-" + std::to_string(sequence);
  const fs::path candidate = experiment_root / game_id;
  if (fs::exists(candidate, error))
    return absl::AlreadyExistsError("Duplicate stitching experiment workspace: " + candidate.string());
  fs::create_directory(candidate, error);
  if (error)
    return absl::InternalError("Unable to create stitching experiment workspace: " + error.message());
  struct CleanupCandidate {
    fs::path path;
    bool retain{false};
    ~CleanupCandidate() {
      if (!retain) {
        std::error_code ignored;
        fs::remove_all(path, ignored);
      }
    }
  } cleanup{candidate};

  try {
    YAML::Node config = YAML::LoadFile(source_config.string());
    if (!config || !config.IsMap())
      return absl::InvalidArgumentError("The selected game config must contain a YAML map");
    std::set<fs::path> linked;
    HM_RETURN_IF_ERROR(link_configured_videos(config, source_game_directory, candidate, &linked));
    HM_RETURN_IF_ERROR(link_auto_videos(source_game_directory, candidate, &linked));
    if (linked.size() < 2)
      return absl::FailedPreconditionError("Stitching experiments require at least two camera videos");
    HM_RETURN_IF_ERROR(link_calibration_assets(source_game_directory, candidate, linked));
    promote_generated_video_roles(config);
    const std::string invalidation_id = "stitch-experiment-" + std::to_string(::getpid()) + "-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(sequence);
    HM_RETURN_IF_ERROR(configure_candidate(config, settings, invalidation_id));
    HM_RETURN_IF_ERROR(copy_selected_input_bundle(source_game_directory, candidate, config));
    HM_RETURN_IF_ERROR(write_config(candidate / "config.yaml", config));
    cleanup.retain = true;
    return StitchingExperimentWorkspace{
        .root = experiment_root,
        .game_directory = candidate,
        .game_id = game_id,
        .invalidation_id = invalidation_id,
        .settings = settings,
    };
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to prepare stitching experiment config: " + std::string(exception.what()));
  }
}

absl::Status PromoteStitchingExperiment(
    const StitchingExperimentWorkspace& experiment,
    const fs::path& game_directory) {
  return hm::stitching::HuginProject::PromoteArtifactsAndConfig(
      experiment.game_directory, game_directory, [&]() -> absl::StatusOr<std::string> {
        std::string selected;
        HM_ASSIGN_OR_RETURN(
            selected,
            BuildStitchingExperimentSelectionConfig(
                experiment.game_directory / "config.yaml", game_directory / "config.yaml"));
        try {
          HM_RETURN_IF_ERROR(
              copy_ordinary_inspection_for_promotion(experiment.game_directory, game_directory, YAML::Load(selected)));
        } catch (const YAML::Exception& error) {
          return absl::InvalidArgumentError("Invalid promoted frame inspection: " + std::string(error.what()));
        }
        return selected;
      });
}

absl::StatusOr<std::string> BuildStitchingExperimentSelectionConfig(
    const fs::path& experiment_config,
    const fs::path& game_config) {
  try {
    YAML::Node selected = YAML::LoadFile(experiment_config.string());
    YAML::Node current = YAML::LoadFile(game_config.string());
    HM_RETURN_IF_ERROR(reconcile_selected_video_paths(current, selected, game_config.parent_path()));
    YAML::Node selected_stitching = selected["stitching"];
    YAML::Node current_stitching = current["stitching"];
    for (const char* key :
         {"calibration_frame_count",
          "calibration_frame_selection",
          "calibration_frame_inputs_fingerprint",
          "camera_config",
          "camera_fov",
          "control_point_execution_provider",
          "control_point_matcher",
          "control_point_resolution",
          "mapping_backend",
          "max_output_width",
          "projection",
          "projection_framing",
          "projection_parameters",
          "run_autooptimizer",
          "stitch_frame_time"}) {
      copy_node(current_stitching, selected_stitching, key);
    }
    copy_node(current["game"]["stitching"], selected["game"]["stitching"], "frame_offsets");
    current["hstream_ui"]["stitching_calibration"] = YAML::Clone(selected["hstream_ui"]["stitching_calibration"]);
    YAML::Node calibration = current["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "complete";
    calibration["rink_mask_status"] = "pending";
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    copy_node(current["hstream_ui"], selected["hstream_ui"], "generated_stitching_backend_choices");
    // Choosing this candidate accepts its existing crop as seen in the preview.
    // Its alignment may differ from the inherited game's crop-review marker.
    // Bind the review to the promoted geometry so first Play reuses these maps.
    current["hstream_ui"].remove("projection_crop_geometry");
    const fs::path project_path = experiment_config.parent_path() / "autooptimiser_out.pto";
    std::error_code project_error;
    const bool has_project = fs::is_regular_file(project_path, project_error);
    if (project_error && project_error != std::errc::no_such_file_or_directory)
      return absl::InternalError(
          "Unable to inspect the selected candidate's crop geometry: " + project_error.message());
    if (has_project) {
      std::ifstream project(project_path);
      if (!project)
        return absl::InternalError("Unable to open the selected candidate's crop geometry");
      std::string pto(1024 * 1024 + 1, '\0');
      project.read(pto.data(), pto.size());
      pto.resize(project.gcount());
      if (project.bad())
        return absl::InternalError("Unable to read the selected candidate's crop geometry");
      if (pto.size() > 1024 * 1024)
        return absl::FailedPreconditionError("Selected candidate project exceeds the crop-review size limit");
      hm::stitching::StitchProjectionFraming framing;
      HM_ASSIGN_OR_RETURN(framing, hm::stitching::read_stitch_projection_framing(selected));
      const std::string geometry = hm::stitching::projection_crop_geometry(pto, framing);
      if (geometry.empty())
        return absl::FailedPreconditionError("Selected candidate has no valid crop geometry");
      hm::stitching::write_projection_crop_review(current, geometry);
    }
    remove_downstream_generation(current);
    return YAML::Dump(current) + "\n";
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to promote stitching experiment config: " + std::string(exception.what()));
  }
}

absl::Status CompleteStitchingExperimentWorkspace(
    const StitchingExperimentWorkspace& experiment,
    bool require_ice_mask) {
  std::optional<std::string> mask_generation;
  if (require_ice_mask) {
    HM_ASSIGN_OR_RETURN(
        mask_generation, hm::stitching::current_stitched_output_generation_id(experiment.game_directory));
    auto mask = hm::stitching::load_field_mask(experiment.game_directory, *mask_generation, experiment.invalidation_id);
    if (!mask.ok())
      return mask.status();
  }
  auto lock = hm::stitching::HuginProject::RecoverAndLock(experiment.game_directory);
  if (!lock.ok())
    return lock.status();
  HM_RETURN_IF_ERROR(hm::stitching::validate_stitch_generation_artifact_bounds_locked(experiment.game_directory));
  auto generation = hm::stitching::stitch_artifact_generation_id_locked(experiment.game_directory);
  if (!generation.ok())
    return generation.status();
  auto config_lock = hm::stitching::GameConfigTransactionLock::Acquire(experiment.game_directory);
  if (!config_lock.ok())
    return config_lock.status();
  try {
    YAML::Node config = YAML::LoadFile((experiment.game_directory / "config.yaml").string());
    HM_RETURN_IF_ERROR(hm::stitching::validate_stitching_generation_owner(config, experiment.invalidation_id));
    if (mask_generation) {
      HM_RETURN_IF_ERROR(hm::stitching::validate_stitched_output_generation_hugin(*mask_generation, *generation));
      if (config["rink"]["stitched_output_generation"].as<std::string>("") != *mask_generation)
        return absl::AbortedError("The experiment rink mask changed before bootstrap completion");
    }
    // Calibration-only runners leave UI completion persistence to their owner.
    // Leaving this pending makes the next preview clean and regenerate its maps.
    YAML::Node calibration = config["hstream_ui"]["stitching_calibration"];
    calibration["status"] = "complete";
    calibration["rink_mask_status"] = require_ice_mask ? "complete" : "omitted";
    calibration.remove("stale_from");
    calibration.remove("artifacts_invalidated");
    return hm::stitching::publish_game_config(experiment.game_directory, YAML::Dump(config) + "\n");
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to complete stitching experiment: " + std::string(exception.what()));
  }
}

absl::StatusOr<StitchingExperimentPlayerSelection> PreparePlayerSelectedStitchingExperiment(
    const StitchingExperimentWorkspace& baseline,
    const StitchingExperimentWorkspace& candidate,
    const fs::path& report_path,
    int search_duration_seconds) {
  if (search_duration_seconds < 1 || search_duration_seconds > 300)
    return absl::InvalidArgumentError("Player search duration must be between 1 and 300 seconds");
  hm::stitching::PlayerFrameSelectionReport report;
  HM_ASSIGN_OR_RETURN(report, hm::stitching::LoadPlayerFrameSelectionReport(report_path));
  if (!report.plan)
    return StitchingExperimentPlayerSelection{false, report.unavailable_reason};
  const auto& plan = *report.plan;
  if (plan.settings.frame_count != static_cast<size_t>(candidate.settings.frame_count) ||
      plan.settings.duration_ns != static_cast<uint64_t>(search_duration_seconds) * hm::stitching::kPlayerFrameSecond ||
      plan.settings.interval_ns != hm::stitching::kPlayerFrameSecond / 2 ||
      baseline.settings.frame_count != candidate.settings.frame_count) {
    return absl::InvalidArgumentError("Player scan and candidate calibration settings differ");
  }
  HM_RETURN_IF_ERROR(hm::stitching::ValidatePlayerFrameSources(plan));
  const auto anchor_value = plan.context.find("decode_anchor_ns");
  if (anchor_value == plan.context.end() || anchor_value->second.empty() ||
      !std::all_of(anchor_value->second.begin(), anchor_value->second.end(), [](unsigned char value) {
        return std::isdigit(value);
      })) {
    return absl::InvalidArgumentError("Player scan does not identify its decode anchor");
  }
  uint64_t anchor_ns = 0;
  try {
    anchor_ns = std::stoull(anchor_value->second);
  } catch (const std::exception&) {
    return absl::InvalidArgumentError("Player scan decode anchor is out of range");
  }
  try {
    if (anchor_ns != hm::stitch_frame_time_to_nanoseconds(candidate.settings.stitch_frame_time))
      return absl::AbortedError("Player scan used a different anchor from its candidate");
  } catch (const std::exception&) {
    return absl::InvalidArgumentError("The candidate has an invalid calibration anchor");
  }

  // Validate the existing mask through its publication locks before taking the
  // artifact/config locks below; the field-mask reader acquires those itself.
  auto mask = hm::stitching::load_field_mask(
      baseline.game_directory, plan.context.at("output_generation"), baseline.invalidation_id);
  if (!mask.ok())
    return mask.status();
  std::string mask_fingerprint;
  HM_ASSIGN_OR_RETURN(mask_fingerprint, hm::stitching::PlayerFrameMaskFingerprint(*mask));
  if (mask_fingerprint != plan.context.at("rink_mask_sha256"))
    return absl::AbortedError("The baseline rink mask changed after player scanning");

  auto artifact_lock = hm::stitching::HuginProject::RecoverAndLock(baseline.game_directory);
  if (!artifact_lock.ok())
    return artifact_lock.status();
  std::string baseline_generation;
  HM_ASSIGN_OR_RETURN(
      baseline_generation, hm::stitching::HuginProject::GenerationId(baseline.game_directory, **artifact_lock));
  if (baseline_generation != plan.context.at("baseline_generation"))
    return absl::AbortedError("The baseline geometry changed after player scanning");
  auto baseline_config_lock = hm::stitching::GameConfigTransactionLock::Acquire(baseline.game_directory);
  if (!baseline_config_lock.ok())
    return baseline_config_lock.status();
  auto candidate_config_lock = hm::stitching::GameConfigTransactionLock::Acquire(candidate.game_directory);
  if (!candidate_config_lock.ok())
    return candidate_config_lock.status();
  try {
    const YAML::Node source_config = YAML::LoadFile((baseline.game_directory / "config.yaml").string());
    YAML::Node config = YAML::LoadFile((candidate.game_directory / "config.yaml").string());
    HM_RETURN_IF_ERROR(hm::stitching::validate_stitching_generation_owner(source_config, baseline.invalidation_id));
    HM_RETURN_IF_ERROR(hm::stitching::validate_pending_stitching_invalidation(config, candidate.invalidation_id));
    if (source_config["hstream_ui"]["stitching_calibration"]["status"].as<std::string>("") != "complete" ||
        source_config["rink"]["stitched_output_generation"].as<std::string>("") !=
            plan.context.at("output_generation")) {
      return absl::AbortedError("The baseline is no longer complete for the player scan geometry");
    }
    std::string expected_context;
    HM_ASSIGN_OR_RETURN(expected_context, hm::stitching::player_frame_source_context(source_config, anchor_ns));
    HM_RETURN_IF_ERROR(hm::stitching::ValidatePlayerFrameSourceContext(plan, expected_context));

    // The baseline runner has already resolved inherited camera settings. The
    // pending candidate has not run yet, so freeze that same effective selection
    // before validating its context instead of falling back to bundled defaults.
    // Reference-time reconciliation may remove a redundant inherited timestamp;
    // retain the same persisted form (including absence). The actual decode
    // anchor was independently checked against the candidate settings above.
    YAML::Node selected_source = YAML::Clone(source_config);
    selected_source["stitching"]["calibration_frame_selection"] = hm::stitching::PlayerFrameSelectionPlanYaml(plan);
    HM_RETURN_IF_ERROR(copy_selected_frame_inputs(
        config, selected_source, baseline.game_directory, candidate.game_directory, /*freeze_camera=*/true));
    HM_ASSIGN_OR_RETURN(expected_context, hm::stitching::player_frame_source_context(config, anchor_ns));
    HM_RETURN_IF_ERROR(hm::stitching::ValidatePlayerFrameSourceContext(plan, expected_context));
    config["stitching"]["calibration_frame_selection"] = hm::stitching::PlayerFrameSelectionPlanYaml(plan);
    HM_RETURN_IF_ERROR(hm::stitching::publish_game_config(candidate.game_directory, YAML::Dump(config) + "\n"));
    return StitchingExperimentPlayerSelection{
        true,
        "Selected " + std::to_string(plan.selected.size()) + " synchronized pairs from " +
            std::to_string(report.observation_count) +
            " samples; far/middle/near observations: " + std::to_string(report.observed_size_band_counts[0]) + "/" +
            std::to_string(report.observed_size_band_counts[1]) + "/" +
            std::to_string(report.observed_size_band_counts[2])};
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError("Unable to freeze player frame selection: " + std::string(exception.what()));
  }
}

absl::Status ReuseStitchingExperimentPlayerSelection(
    const StitchingExperimentWorkspace& source,
    const StitchingExperimentWorkspace& candidate) {
  if (source.game_directory == candidate.game_directory)
    return absl::InvalidArgumentError("Frame selection reuse requires a separate candidate");
  auto source_lock = hm::stitching::GameConfigTransactionLock::Acquire(source.game_directory);
  if (!source_lock.ok())
    return source_lock.status();
  auto candidate_lock = hm::stitching::GameConfigTransactionLock::Acquire(candidate.game_directory);
  if (!candidate_lock.ok())
    return candidate_lock.status();
  try {
    const YAML::Node source_config = YAML::LoadFile((source.game_directory / "config.yaml").string());
    YAML::Node config = YAML::LoadFile((candidate.game_directory / "config.yaml").string());
    if (source.invalidation_id.empty() ||
        source_config["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>("") !=
            source.invalidation_id)
      return absl::AbortedError("The frame selection owner was superseded");
    HM_RETURN_IF_ERROR(hm::stitching::validate_pending_stitching_invalidation(config, candidate.invalidation_id));
    std::string fingerprint;
    HM_ASSIGN_OR_RETURN(fingerprint, reusable_selection_fingerprint(source_config, candidate.settings));
    if (fingerprint.empty())
      return absl::FailedPreconditionError("The frame selection owner has no compatible saved frames");
    HM_RETURN_IF_ERROR(hm::stitching::validate_player_frame_selection_sources(source_config));
    HM_RETURN_IF_ERROR(copy_selected_frame_inputs(
        config, source_config, source.game_directory, candidate.game_directory, /*freeze_camera=*/false));
    HM_RETURN_IF_ERROR(copy_selected_input_bundle(source.game_directory, candidate.game_directory, config));
    return hm::stitching::publish_game_config(candidate.game_directory, YAML::Dump(config) + "\n");
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError("Unable to reuse saved player frames: " + std::string(error.what()));
  }
}

absl::StatusOr<StitchingExperimentFrameInspection> InspectStitchingExperimentFrames(
    const StitchingExperimentWorkspace& experiment) {
  auto lock = hm::stitching::GameConfigTransactionLock::Acquire(experiment.game_directory);
  if (!lock.ok())
    return lock.status();
  try {
    const YAML::Node config = YAML::LoadFile((experiment.game_directory / "config.yaml").string());
    // A failed or cancelled solve still has useful frozen choices to inspect.
    // Unlike publication, this read-only path only needs the workspace identity.
    if (experiment.invalidation_id.empty() ||
        config["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>("") !=
            experiment.invalidation_id)
      return absl::AbortedError("Selected frame workspace was superseded");
    const YAML::Node selection = config["stitching"]["calibration_frame_selection"];
    if (!selection || selection.IsNull()) {
      if (!std::regex_match(experiment.invalidation_id, std::regex("[A-Za-z0-9][A-Za-z0-9_.-]{0,159}")))
        return absl::InvalidArgumentError("Invalid calibration inspection owner");
      const fs::path images = experiment.game_directory / "calibration-frame-inspection" / experiment.invalidation_id;
      std::ifstream file(images / "frames.yaml");
      if (!file)
        return absl::NotFoundError("No captured calibration frames are available for this candidate");
      std::string contents(64 * 1024 + 1, '\0');
      file.read(contents.data(), contents.size());
      contents.resize(file.gcount());
      if (file.bad() || contents.size() > 64 * 1024)
        return absl::InvalidArgumentError("Calibration frame inspection exceeds its read limit");
      const YAML::Node manifest = YAML::Load(contents);
      if (!manifest.IsMap() || manifest["version"].as<int>(0) != 1 ||
          manifest["invalidation_id"].as<std::string>("") != experiment.invalidation_id)
        return absl::InvalidArgumentError("Calibration frame inspection has an invalid owner or version");
      StitchingExperimentFrameInspection result;
      result.expected_pair_count = manifest["expected_pair_count"].as<size_t>(0);
      const YAML::Node pairs = manifest["pairs"];
      if (!result.expected_pair_count || result.expected_pair_count > 16 ||
          result.expected_pair_count != static_cast<size_t>(experiment.settings.frame_count) || !pairs.IsSequence() ||
          pairs.size() > result.expected_pair_count || pairs.size() == 0)
        return absl::InvalidArgumentError("Calibration frame inspection has an invalid pair count");
      const bool complete = config["hstream_ui"]["stitching_calibration"]["status"].as<std::string>("") == "complete";
      if (complete && pairs.size() != result.expected_pair_count)
        return absl::FailedPreconditionError("Completed calibration is missing captured frame inspection pairs");
      for (size_t index = 0; index < pairs.size(); ++index) {
        const YAML::Node pair = pairs[index];
        if (pair["index"].as<size_t>(16) != index)
          return absl::InvalidArgumentError("Calibration frame inspection pair indices are invalid");
        StitchingExperimentSelectedFrame frame;
        for (size_t camera = 0; camera < 2; ++camera) {
          const char* role = camera == 0 ? "left" : "right";
          frame.camera_paths[camera] = pair[role]["path"].as<std::string>();
          frame.source_seconds[camera] = pair[role]["source_seconds"].as<double>();
          if (!std::isfinite(frame.source_seconds[camera]) || frame.source_seconds[camera] < 0)
            return absl::InvalidArgumentError("Calibration frame inspection has an invalid source time");
          frame.thumbnails[camera] = images / (std::string(role) + "_" + std::to_string(index) + ".jpg");
        }
        result.frames.push_back(std::move(frame));
      }
      result.source_validation = "Captured " + std::to_string(result.frames.size()) + " of " +
          std::to_string(result.expected_pair_count) + " calibration pairs for this candidate.";
      if (result.frames.size() != result.expected_pair_count)
        result.source_validation += " Partial capture: the remaining pairs are unavailable.";
      return result;
    }
    hm::stitching::PlayerFrameSelectionPlan plan;
    HM_ASSIGN_OR_RETURN(
        plan, hm::stitching::ParsePlayerFrameSelectionPlan(config["stitching"]["calibration_frame_selection"]));
    StitchingExperimentFrameInspection result;
    result.player_selected = true;
    result.expected_pair_count = plan.selected.size();
    const auto anchor = plan.context.find("decode_anchor_ns");
    if (anchor == plan.context.end())
      return absl::InvalidArgumentError("Selected frames do not identify their decode anchor");
    result.anchor_ns = std::stoull(anchor->second);
    result.fingerprint = plan.fingerprint;
    std::optional<hm::stitching::PlayerFrameInputSet> bundle;
    HM_ASSIGN_OR_RETURN(
        bundle,
        hm::stitching::LoadPlayerFrameInputs(
            experiment.game_directory, plan, hm::stitching::PlayerFrameInputValidation::kInspection));
    const YAML::Node marker = config["stitching"]["calibration_frame_inputs_fingerprint"];
    if (marker && (!marker.IsScalar() || marker.as<std::string>() != plan.fingerprint || !bundle))
      return absl::FailedPreconditionError("The saved frame input bundle is missing or does not match its selection");
    const absl::Status sources = hm::stitching::ValidatePlayerFrameSources(plan);
    result.source_validation =
        sources.ok() ? "Source files still match the recorded selection." : "Source validation: " + sources.ToString();
    const fs::path images = experiment.game_directory / "player-frame-inspection" / plan.fingerprint;
    for (size_t index = 0; index < plan.selected.size(); ++index) {
      const auto& selected = plan.selected[index];
      StitchingExperimentSelectedFrame frame;
      frame.timeline_ns = selected.pair.timeline_pts_ns;
      frame.eligible_people = selected.eligible_people;
      frame.size_band_counts = selected.size_band_counts;
      frame.quality = selected.quality;
      frame.coverage = selected.coverage;
      for (size_t camera = 0; camera < 2; ++camera) {
        frame.camera_paths[camera] = selected.pair.cameras[camera].path;
        frame.source_pts_ns[camera] = selected.pair.cameras[camera].source_pts_ns;
        frame.decoded_sequences[camera] = selected.pair.cameras[camera].sequence;
        frame.thumbnails[camera] = bundle
            ? bundle->thumbnails[index][camera]
            : images / ((camera == 0 ? "left_" : "right_") + std::to_string(index) + ".jpg");
      }
      result.frames.push_back(std::move(frame));
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Unable to inspect selected frames: " + std::string(error.what()));
  }
}
