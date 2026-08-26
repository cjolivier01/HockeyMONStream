#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"

#include "hstream/src/libs/common/Status.h"
#include "hstream/src/libs/stitching/CanvasConstraintCheck.h"
#include "hstream/src/libs/stitching/GameConfig.h"

#include <yaml-cpp/yaml.h>

#include <cctype>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace hm::stitching {
namespace {

absl::StatusOr<size_t> parse_positive_size(std::string_view value, std::string_view field) {
  if (value.empty())
    return absl::InvalidArgumentError("Invalid stitched-output " + std::string(field));
  size_t parsed = 0;
  for (const unsigned char character : value) {
    if (!std::isdigit(character))
      return absl::InvalidArgumentError("Invalid stitched-output " + std::string(field));
    const size_t digit = static_cast<size_t>(character - '0');
    if (parsed > (std::numeric_limits<size_t>::max() - digit) / 10)
      return absl::InvalidArgumentError("Stitched-output " + std::string(field) + " is too large");
    parsed = parsed * 10 + digit;
  }
  if (parsed == 0)
    return absl::InvalidArgumentError("Stitched-output " + std::string(field) + " must be positive");
  return parsed;
}

absl::Status validate_rotation(std::string_view value) {
  if (value.empty())
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  std::istringstream parser{std::string(value)};
  parser.imbue(std::locale::classic());
  double rotation = 0.0;
  parser >> rotation;
  if (!parser || !std::isfinite(rotation))
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  parser >> std::ws;
  if (!parser.eof())
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  return absl::OkStatus();
}

struct EmbeddedHuginGeneration {
  size_t start{0};
  size_t size{0};
};

absl::StatusOr<EmbeddedHuginGeneration> embedded_hugin_generation(const std::string& generation) {
  constexpr std::string_view prefix = "hstream-stitched-output-v1\nhugin-bytes:";
  if (generation.compare(0, prefix.size(), prefix) != 0)
    return absl::InvalidArgumentError("Invalid stitched-output generation header");

  const size_t length_end = generation.find('\n', prefix.size());
  if (length_end == std::string::npos)
    return absl::InvalidArgumentError("Invalid stitched-output Hugin length");
  auto hugin_size = parse_positive_size(
      std::string_view(generation).substr(prefix.size(), length_end - prefix.size()), "Hugin length");
  if (!hugin_size.ok())
    return hugin_size.status();
  const size_t hugin_start = length_end + 1;
  if (*hugin_size > generation.size() - hugin_start)
    return absl::InvalidArgumentError("Truncated stitched-output Hugin generation");
  return EmbeddedHuginGeneration{hugin_start, *hugin_size};
}

absl::StatusOr<std::string> generation_with_rotation(const std::string& generation, double post_stitch_rotate_degrees) {
  constexpr std::string_view rotation_prefix = "post-stitch-rotate-degrees:";
  constexpr std::string_view output_size_prefix = "output-size:";
  EmbeddedHuginGeneration hugin;
  HM_ASSIGN_OR_RETURN(hugin, embedded_hugin_generation(generation));

  const size_t rotation_start = hugin.start + hugin.size;
  if (generation.compare(rotation_start, rotation_prefix.size(), rotation_prefix) != 0)
    return absl::InvalidArgumentError("Invalid stitched-output rotation field");
  const size_t value_start = rotation_start + rotation_prefix.size();
  const size_t value_end = generation.find('\n', value_start);
  if (value_end == std::string::npos)
    return absl::InvalidArgumentError("Invalid stitched-output rotation value");
  const absl::Status rotation_status =
      validate_rotation(std::string_view(generation).substr(value_start, value_end - value_start));
  if (!rotation_status.ok())
    return rotation_status;

  const size_t output_size_start = value_end + 1;
  if (generation.compare(output_size_start, output_size_prefix.size(), output_size_prefix) != 0 || generation.empty() ||
      generation.back() != '\n') {
    return absl::FailedPreconditionError("Live stitched-output rotation requires generation dimensions");
  }
  const size_t dimensions_start = output_size_start + output_size_prefix.size();
  const std::string_view dimensions(generation.data() + dimensions_start, generation.size() - dimensions_start - 1);
  const size_t separator = dimensions.find('x');
  if (separator == std::string_view::npos) {
    return absl::InvalidArgumentError("Invalid stitched-output dimensions");
  }
  auto width = parse_positive_size(dimensions.substr(0, separator), "width");
  if (!width.ok())
    return width.status();
  auto height = parse_positive_size(dimensions.substr(separator + 1), "height");
  if (!height.ok())
    return height.status();

  if (post_stitch_rotate_degrees == 0.0)
    post_stitch_rotate_degrees = 0.0;
  std::ostringstream rotation;
  rotation.imbue(std::locale::classic());
  rotation << std::setprecision(std::numeric_limits<double>::max_digits10) << post_stitch_rotate_degrees;

  std::string authorized;
  authorized.reserve(generation.size() + rotation.str().size());
  authorized.append(generation, 0, value_start);
  authorized.append(rotation.str());
  authorized.append(generation, value_end, std::string::npos);
  return authorized;
}

bool scoreboard_polygon_is_disabled(const YAML::Node& polygon) {
  if (!polygon || !polygon.IsSequence() || polygon.size() != 4)
    return false;
  try {
    for (const YAML::Node& point : polygon) {
      if (!point.IsSequence() || point.size() != 2 || point[0].as<double>() != 0.0 || point[1].as<double>() != 0.0)
        return false;
    }
    return true;
  } catch (const YAML::Exception&) {
    return false;
  }
}

void remove_active_scoreboard_polygon(YAML::Node& config) {
  const YAML::Node polygon = config["rink"]["scoreboard"]["perspective_polygon"];
  if (polygon && polygon.IsDefined() && !scoreboard_polygon_is_disabled(polygon))
    config["rink"]["scoreboard"].remove("perspective_polygon");
}

void remove_pending_authorization(YAML::Node& config) {
  config["rink"].remove("stitched_output_pending_generation");
  config["rink"].remove("stitched_output_pending_authorization_id");
  config["rink"].remove("stitched_output_pending_owner_process");
  config["rink"].remove("stitched_output_pending_previous_generation");
  config["rink"].remove("stitched_output_pending_previous_authorization_id");
  config["rink"].remove("stitched_output_pending_previous_owner_process");
  config["rink"].remove("stitched_output_pending_completed_scoreboard_polygon");
}

void remove_pending_predecessor(YAML::Node& config) {
  config["rink"].remove("stitched_output_pending_previous_generation");
  config["rink"].remove("stitched_output_pending_previous_authorization_id");
  config["rink"].remove("stitched_output_pending_previous_owner_process");
}

absl::StatusOr<std::optional<std::string>> optional_scalar(const YAML::Node& node, std::string_view field) {
  if (!node || !node.IsDefined())
    return std::nullopt;
  if (!node.IsScalar())
    return absl::InvalidArgumentError(std::string(field) + " must be a scalar");
  return node.as<std::string>();
}

absl::StatusOr<std::string> scoreboard_property_value(const YAML::Node& config) {
  const YAML::Node active_polygon = config["rink"]["scoreboard"]["perspective_polygon"];
  const YAML::Node saved_polygon = config["rink"]["stitched_output_pending_completed_scoreboard_polygon"];
  const YAML::Node polygon = active_polygon && active_polygon.IsDefined() ? active_polygon : saved_polygon;
  if (!polygon || !polygon.IsDefined())
    return std::string("0,0,0,0,0,0,0,0");
  if (!polygon.IsSequence() || polygon.size() != 4)
    return absl::InvalidArgumentError("Scoreboard perspective polygon must contain four points");
  std::ostringstream value;
  value.imbue(std::locale::classic());
  value << std::setprecision(std::numeric_limits<double>::max_digits10);
  bool first = true;
  for (const YAML::Node& point : polygon) {
    if (!point.IsSequence() || point.size() != 2)
      return absl::InvalidArgumentError("Scoreboard perspective polygon points must contain two coordinates");
    for (const YAML::Node& coordinate : point) {
      const double parsed = coordinate.as<double>();
      if (!std::isfinite(parsed))
        return absl::InvalidArgumentError("Scoreboard perspective polygon coordinates must be finite");
      if (!first)
        value << ',';
      first = false;
      value << parsed;
    }
  }
  return value.str();
}

} // namespace

absl::StatusOr<LiveStitchedOutputAuthorization> authorize_live_stitched_output_rotation(
    const std::string& game_dir,
    double post_stitch_rotate_degrees,
    const std::string& authorization_id) {
  if (game_dir.empty() || !std::isfinite(post_stitch_rotate_degrees) || authorization_id.empty()) {
    return absl::InvalidArgumentError(
        "A game directory, finite live stitched-output rotation, and unique authorization ID are required");
  }
  const fs::path root(game_dir);
  auto hugin_lock = lock_canvas_constraint_artifacts(root);
  if (!hugin_lock.ok())
    return hugin_lock.status();
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();

  const fs::path config_path = root / "config.yaml";
  std::error_code error;
  const bool has_config = fs::is_regular_file(config_path, error);
  if (error)
    return absl::InternalError("Unable to inspect game config: " + error.message());
  if (!has_config)
    return LiveStitchedOutputAuthorization{};

  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    if (!config || !config.IsMap())
      return absl::InvalidArgumentError("Game config must be a map");
    const YAML::Node saved_generation = config["rink"]["stitched_output_generation"];
    if (!saved_generation || !saved_generation.IsDefined())
      return LiveStitchedOutputAuthorization{};
    if (!saved_generation.IsScalar())
      return absl::InvalidArgumentError("Persisted stitched-output generation must be a scalar");

    auto authorized_generation =
        generation_with_rotation(saved_generation.as<std::string>(), post_stitch_rotate_degrees);
    if (!authorized_generation.ok())
      return authorized_generation.status();
    EmbeddedHuginGeneration embedded_hugin;
    HM_ASSIGN_OR_RETURN(embedded_hugin, embedded_hugin_generation(saved_generation.as<std::string>()));
    auto current_hugin = stitch_artifact_generation_id_locked(root);
    if (!current_hugin.ok())
      return current_hugin.status();
    const std::string& persisted_generation = saved_generation.as<std::string>();
    if (persisted_generation.compare(embedded_hugin.start, embedded_hugin.size, *current_hugin) != 0 ||
        current_hugin->size() != embedded_hugin.size) {
      return absl::AbortedError("Persisted stitched-output generation does not match the current Hugin artifacts");
    }
    std::string owner_process;
    HM_ASSIGN_OR_RETURN(owner_process, current_live_stitched_output_owner_process());
    std::optional<std::string> previous_generation;
    HM_ASSIGN_OR_RETURN(
        previous_generation,
        optional_scalar(config["rink"]["stitched_output_pending_generation"], "Pending stitched-output generation"));
    std::optional<std::string> previous_authorization_id;
    HM_ASSIGN_OR_RETURN(
        previous_authorization_id,
        optional_scalar(
            config["rink"]["stitched_output_pending_authorization_id"], "Pending stitched-output authorization ID"));
    if (previous_generation.has_value() != previous_authorization_id.has_value()) {
      // Authorization IDs were added after generation fencing. A lone legacy
      // generation cannot be restored with ABA safety, so retire it before
      // creating the first epoch-backed authorization.
      previous_generation.reset();
      previous_authorization_id.reset();
      remove_pending_authorization(config);
    }
    if (previous_generation.has_value()) {
      bool previous_authorization_is_active = false;
      HM_ASSIGN_OR_RETURN(previous_authorization_is_active, live_stitched_output_authorization_is_active(config));
      if (!previous_authorization_is_active) {
        const YAML::Node completed_scoreboard_polygon =
            config["rink"]["stitched_output_pending_completed_scoreboard_polygon"];
        const YAML::Node active_scoreboard_polygon = config["rink"]["scoreboard"]["perspective_polygon"];
        if ((!active_scoreboard_polygon || !active_scoreboard_polygon.IsDefined()) && completed_scoreboard_polygon &&
            completed_scoreboard_polygon.IsDefined()) {
          config["rink"]["scoreboard"]["perspective_polygon"] = YAML::Clone(completed_scoreboard_polygon);
        }
        remove_pending_authorization(config);
        previous_generation.reset();
        previous_authorization_id.reset();
      }
    }
    std::optional<std::string> previous_owner_process;
    if (previous_generation.has_value()) {
      HM_ASSIGN_OR_RETURN(
          previous_owner_process,
          optional_scalar(
              config["rink"]["stitched_output_pending_owner_process"], "Pending stitched-output owner process"));
      if (!previous_owner_process.has_value() || *previous_owner_process != owner_process) {
        return absl::AbortedError("Another process owns the pending live stitched-output authorization");
      }
    }

    const bool generation_changed = *authorized_generation != saved_generation.as<std::string>();
    if (!generation_changed && !previous_generation.has_value()) {
      remove_pending_authorization(config);
      const absl::Status published = publish_game_config(root, YAML::Dump(config) + "\n");
      if (!published.ok())
        return published;
      return LiveStitchedOutputAuthorization{};
    }

    config["rink"]["stitched_output_pending_generation"] = *authorized_generation;
    config["rink"]["stitched_output_pending_authorization_id"] = authorization_id;
    config["rink"]["stitched_output_pending_owner_process"] = owner_process;
    const YAML::Node active_scoreboard_polygon = config["rink"]["scoreboard"]["perspective_polygon"];
    if (!previous_generation.has_value() && active_scoreboard_polygon && active_scoreboard_polygon.IsDefined()) {
      config["rink"]["stitched_output_pending_completed_scoreboard_polygon"] = YAML::Clone(active_scoreboard_polygon);
    }
    if (previous_generation.has_value()) {
      config["rink"]["stitched_output_pending_previous_generation"] = *previous_generation;
      if (previous_authorization_id.has_value()) {
        config["rink"]["stitched_output_pending_previous_authorization_id"] = *previous_authorization_id;
      } else {
        config["rink"].remove("stitched_output_pending_previous_authorization_id");
      }
      if (previous_owner_process.has_value()) {
        config["rink"]["stitched_output_pending_previous_owner_process"] = *previous_owner_process;
      } else {
        config["rink"].remove("stitched_output_pending_previous_owner_process");
      }
    } else {
      config["rink"].remove("stitched_output_pending_previous_generation");
      config["rink"].remove("stitched_output_pending_previous_authorization_id");
      config["rink"].remove("stitched_output_pending_previous_owner_process");
    }
    std::string runtime_scoreboard_value = "0,0,0,0,0,0,0,0";
    if (!generation_changed)
      HM_ASSIGN_OR_RETURN(runtime_scoreboard_value, scoreboard_property_value(config));
    const absl::Status published = publish_game_config(root, YAML::Dump(config) + "\n");
    if (!published.ok())
      return published;
    return LiveStitchedOutputAuthorization{
        *authorized_generation, authorization_id, generation_changed, runtime_scoreboard_value};
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to authorize live stitched-output rotation: " + std::string(exception.what()));
  }
}

absl::StatusOr<std::optional<LiveStitchedOutputAuthorization>> rollback_live_stitched_output_rotation(
    const std::string& game_dir,
    const std::string& pending_generation,
    const std::string& authorization_id) {
  if (game_dir.empty() || pending_generation.empty() || authorization_id.empty()) {
    return absl::InvalidArgumentError(
        "A game directory and exact pending stitched-output generation authorization are required");
  }
  const fs::path root(game_dir);
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();

  const fs::path config_path = root / "config.yaml";
  std::error_code error;
  const bool has_config = fs::is_regular_file(config_path, error);
  if (error)
    return absl::InternalError("Unable to inspect game config: " + error.message());
  if (!has_config)
    return std::nullopt;

  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    if (!config || !config.IsMap())
      return absl::InvalidArgumentError("Game config must be a map");
    std::optional<std::string> saved_pending_generation;
    HM_ASSIGN_OR_RETURN(
        saved_pending_generation,
        optional_scalar(config["rink"]["stitched_output_pending_generation"], "Pending stitched-output generation"));
    std::optional<std::string> saved_authorization_id;
    HM_ASSIGN_OR_RETURN(
        saved_authorization_id,
        optional_scalar(
            config["rink"]["stitched_output_pending_authorization_id"], "Pending stitched-output authorization ID"));
    if (!saved_pending_generation.has_value() && !saved_authorization_id.has_value())
      return std::nullopt;
    if (!saved_pending_generation.has_value() || !saved_authorization_id.has_value())
      return absl::InvalidArgumentError("Pending stitched-output authorization is incomplete");
    if (*saved_pending_generation != pending_generation || *saved_authorization_id != authorization_id)
      return absl::AbortedError("Pending stitched-output authorization no longer matches the rollback request");

    std::optional<std::string> previous_generation;
    HM_ASSIGN_OR_RETURN(
        previous_generation,
        optional_scalar(
            config["rink"]["stitched_output_pending_previous_generation"],
            "Previous pending stitched-output generation"));
    std::optional<std::string> previous_authorization_id;
    HM_ASSIGN_OR_RETURN(
        previous_authorization_id,
        optional_scalar(
            config["rink"]["stitched_output_pending_previous_authorization_id"],
            "Previous pending stitched-output authorization ID"));
    std::optional<std::string> previous_owner_process;
    HM_ASSIGN_OR_RETURN(
        previous_owner_process,
        optional_scalar(
            config["rink"]["stitched_output_pending_previous_owner_process"],
            "Previous pending stitched-output owner process"));
    const YAML::Node completed_scoreboard_polygon =
        YAML::Clone(config["rink"]["stitched_output_pending_completed_scoreboard_polygon"]);
    remove_pending_authorization(config);
    std::optional<LiveStitchedOutputAuthorization> restored;
    if (previous_generation.has_value() && previous_authorization_id.has_value() &&
        previous_owner_process.has_value()) {
      config["rink"]["stitched_output_pending_generation"] = *previous_generation;
      config["rink"]["stitched_output_pending_authorization_id"] = *previous_authorization_id;
      config["rink"]["stitched_output_pending_owner_process"] = *previous_owner_process;
      if (completed_scoreboard_polygon && completed_scoreboard_polygon.IsDefined()) {
        config["rink"]["stitched_output_pending_completed_scoreboard_polygon"] = completed_scoreboard_polygon;
      }
      restored = LiveStitchedOutputAuthorization{*previous_generation, *previous_authorization_id, true, {}};
    } else if (completed_scoreboard_polygon && completed_scoreboard_polygon.IsDefined()) {
      const YAML::Node active_scoreboard_polygon = config["rink"]["scoreboard"]["perspective_polygon"];
      if (!active_scoreboard_polygon || !active_scoreboard_polygon.IsDefined())
        config["rink"]["scoreboard"]["perspective_polygon"] = completed_scoreboard_polygon;
    }
    const absl::Status published = publish_game_config(root, YAML::Dump(config) + "\n");
    if (!published.ok())
      return published;
    return restored;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to roll back live stitched-output rotation: " + std::string(exception.what()));
  }
}

absl::Status commit_live_stitched_output_rotation(
    const std::string& game_dir,
    const std::string& pending_generation,
    const std::string& authorization_id) {
  if (game_dir.empty() || pending_generation.empty() || authorization_id.empty()) {
    return absl::InvalidArgumentError(
        "A game directory and exact pending stitched-output generation authorization are required");
  }
  const fs::path root(game_dir);
  auto config_transaction = GameConfigTransactionLock::Acquire(root);
  if (!config_transaction.ok())
    return config_transaction.status();

  const fs::path config_path = root / "config.yaml";
  std::error_code error;
  const bool has_config = fs::is_regular_file(config_path, error);
  if (error)
    return absl::InternalError("Unable to inspect game config: " + error.message());
  if (!has_config)
    return absl::FailedPreconditionError("Cannot commit live stitched-output rotation without a game config");

  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    if (!config || !config.IsMap())
      return absl::InvalidArgumentError("Game config must be a map");
    const YAML::Node saved_generation = config["rink"]["stitched_output_generation"];
    if (!saved_generation || !saved_generation.IsDefined() || !saved_generation.IsScalar()) {
      return absl::FailedPreconditionError(
          "Cannot commit live stitched-output rotation without a completed generation");
    }
    std::optional<std::string> pending;
    HM_ASSIGN_OR_RETURN(
        pending,
        optional_scalar(config["rink"]["stitched_output_pending_generation"], "Pending stitched-output generation"));
    std::optional<std::string> pending_authorization_id;
    HM_ASSIGN_OR_RETURN(
        pending_authorization_id,
        optional_scalar(
            config["rink"]["stitched_output_pending_authorization_id"], "Pending stitched-output authorization ID"));
    const bool producer_committed = saved_generation.as<std::string>() == pending_generation;
    const bool authorization_matches = pending.has_value() && pending_authorization_id.has_value() &&
        *pending == pending_generation && *pending_authorization_id == authorization_id;
    if ((pending.has_value() || pending_authorization_id.has_value()) && !authorization_matches)
      return absl::AbortedError("Pending stitched-output generation no longer matches the live rotation request");
    if (!pending.has_value() && !pending_authorization_id.has_value())
      return producer_committed
          ? absl::OkStatus()
          : absl::AbortedError("Pending stitched-output generation no longer matches the live rotation request");
    if (producer_committed) {
      const YAML::Node active_scoreboard_polygon = config["rink"]["scoreboard"]["perspective_polygon"];
      const YAML::Node completed_scoreboard_polygon =
          config["rink"]["stitched_output_pending_completed_scoreboard_polygon"];
      if ((!active_scoreboard_polygon || !active_scoreboard_polygon.IsDefined()) && completed_scoreboard_polygon &&
          completed_scoreboard_polygon.IsDefined()) {
        config["rink"]["scoreboard"]["perspective_polygon"] = YAML::Clone(completed_scoreboard_polygon);
      }
      remove_pending_authorization(config);
    } else {
      remove_active_scoreboard_polygon(config);
      remove_pending_predecessor(config);
    }
    return publish_game_config(root, YAML::Dump(config) + "\n");
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to commit live stitched-output rotation: " + std::string(exception.what()));
  }
}

} // namespace hm::stitching
