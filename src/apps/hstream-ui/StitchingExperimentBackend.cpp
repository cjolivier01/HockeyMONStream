#include "src/apps/hstream-ui/StitchingExperimentBackend.h"

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/ConfigureStitching.h"
#include "hstream/src/libs/stitching/GameConfig.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/PlayerFrameSelection.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
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

absl::Status configure_candidate(
    YAML::Node config,
    const StitchingExperimentSettings& settings,
    const std::string& invalidation_id) {
  config["stitching"]["calibration_frame_count"] = settings.frame_count;
  config["stitching"]["stitch_frame_time"] = settings.stitch_frame_time;
  // Every ordinary baseline starts independently of any selection previously
  // promoted into the game. Automatic candidates receive their plan only after
  // their own baseline scan has completed and validated.
  config["stitching"].remove("calibration_frame_selection");
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

} // namespace

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
        return BuildStitchingExperimentSelectionConfig(
            experiment.game_directory / "config.yaml", game_directory / "config.yaml");
      });
}

absl::StatusOr<std::string> BuildStitchingExperimentSelectionConfig(
    const fs::path& experiment_config,
    const fs::path& game_config) {
  try {
    YAML::Node selected = YAML::LoadFile(experiment_config.string());
    YAML::Node current = YAML::LoadFile(game_config.string());
    YAML::Node selected_stitching = selected["stitching"];
    YAML::Node current_stitching = current["stitching"];
    for (const char* key :
         {"calibration_frame_count",
          "calibration_frame_selection",
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
      baseline.settings.frame_count != candidate.settings.frame_count ||
      baseline.settings.control_points != candidate.settings.control_points ||
      baseline.settings.stitch_frame_time != candidate.settings.stitch_frame_time ||
      baseline.settings.rink_rotation_degrees != candidate.settings.rink_rotation_degrees) {
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

    YAML::Node videos = YAML::Clone(source_config["game"]["videos"]);
    for (const char* role : {"left", "right"}) {
      YAML::Node paths = videos[role];
      if (!paths.IsSequence() || paths.size() == 0)
        return absl::InvalidArgumentError("The baseline has no resolved camera playlist");
      for (size_t index = 0; index < paths.size(); ++index) {
        const fs::path path = paths[index].as<std::string>();
        const fs::path relative = path.is_absolute() ? path.lexically_relative(baseline.game_directory) : path;
        if (relative.empty() || relative.is_absolute() || *relative.begin() == "..")
          return absl::InvalidArgumentError("A baseline camera path cannot be localized to the candidate");
        std::error_code error;
        if (!fs::equivalent(baseline.game_directory / relative, candidate.game_directory / relative, error) || error)
          return absl::AbortedError("Candidate media differ from the scanned baseline");
        paths[index] = relative.generic_string();
      }
      config["hstream_ui"]["video_roles"][role] = YAML::Clone(paths);
    }
    config["game"]["videos"] = videos;
    const YAML::Node offsets = source_config["game"]["stitching"]["frame_offsets"]
        ? source_config["game"]["stitching"]["frame_offsets"]
        : source_config["stitching"]["frame_offsets"];
    config["game"]["stitching"]["frame_offsets"] = YAML::Clone(offsets);
    config["stitching"].remove("frame_offsets");
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
    hm::stitching::PlayerFrameSelectionPlan plan;
    HM_ASSIGN_OR_RETURN(
        plan, hm::stitching::ParsePlayerFrameSelectionPlan(config["stitching"]["calibration_frame_selection"]));
    StitchingExperimentFrameInspection result;
    const auto anchor = plan.context.find("decode_anchor_ns");
    if (anchor == plan.context.end())
      return absl::InvalidArgumentError("Selected frames do not identify their decode anchor");
    result.anchor_ns = std::stoull(anchor->second);
    result.fingerprint = plan.fingerprint;
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
        frame.thumbnails[camera] = images / ((camera == 0 ? "left_" : "right_") + std::to_string(index) + ".jpg");
      }
      result.frames.push_back(std::move(frame));
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError("Unable to inspect selected frames: " + std::string(error.what()));
  }
}
