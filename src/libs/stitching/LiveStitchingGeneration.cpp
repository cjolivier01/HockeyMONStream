#include "hstream/src/libs/stitching/LiveStitchingGeneration.h"

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

absl::StatusOr<std::string> generation_with_rotation(const std::string& generation, double post_stitch_rotate_degrees) {
  constexpr std::string_view prefix = "hstream-stitched-output-v1\nhugin-bytes:";
  constexpr std::string_view rotation_prefix = "post-stitch-rotate-degrees:";
  constexpr std::string_view output_size_prefix = "output-size:";
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

  const size_t rotation_start = hugin_start + *hugin_size;
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

} // namespace

absl::StatusOr<bool> authorize_live_stitched_output_rotation(
    const std::string& game_dir,
    double post_stitch_rotate_degrees) {
  if (game_dir.empty() || !std::isfinite(post_stitch_rotate_degrees))
    return absl::InvalidArgumentError("A game directory and finite live stitched-output rotation are required");
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
    return false;

  try {
    YAML::Node config = YAML::LoadFile(config_path.string());
    if (!config || !config.IsMap())
      return absl::InvalidArgumentError("Game config must be a map");
    const YAML::Node saved_generation = config["rink"]["stitched_output_generation"];
    if (!saved_generation || !saved_generation.IsDefined())
      return false;
    if (!saved_generation.IsScalar())
      return absl::InvalidArgumentError("Persisted stitched-output generation must be a scalar");

    auto authorized_generation =
        generation_with_rotation(saved_generation.as<std::string>(), post_stitch_rotate_degrees);
    if (!authorized_generation.ok())
      return authorized_generation.status();
    const bool generation_changed = *authorized_generation != saved_generation.as<std::string>();
    if (!generation_changed) {
      config["rink"].remove("stitched_output_pending_generation");
    } else {
      config["rink"]["stitched_output_pending_generation"] = *authorized_generation;
      remove_active_scoreboard_polygon(config);
    }
    const absl::Status published = publish_game_config(root, YAML::Dump(config) + "\n");
    if (!published.ok())
      return published;
    return generation_changed;
  } catch (const YAML::Exception& exception) {
    return absl::InvalidArgumentError(
        "Unable to authorize live stitched-output rotation: " + std::string(exception.what()));
  }
}

} // namespace hm::stitching
