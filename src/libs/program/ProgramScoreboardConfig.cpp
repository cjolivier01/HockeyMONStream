#include "hstream/src/libs/program/ProgramScoreboardConfig.h"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::program {
namespace {
int Rank(const config::ProgramConfiguration& config, const std::string& path) {
  const auto found = config.ranks.find(path);
  return found == config.ranks.end() ? 0 : found->second;
}
YAML::Node Select(const config::ProgramConfiguration& config, const std::vector<std::string>& paths) {
  YAML::Node selected;
  int rank = -1;
  for (const auto& path : paths) {
    const auto value = get_node(config.values, path);
    if (value && Rank(config, path) >= rank) {
      selected.reset(YAML::Clone(*value));
      rank = Rank(config, path);
    }
  }
  return selected;
}
YAML::Node CropSetting(
    const config::ProgramConfiguration& config,
    const std::string& canonical,
    const std::string& native) {
  std::vector<std::string> paths;
  if (!canonical.empty())
    paths.push_back(canonical);
  std::string underscored = native;
  std::replace(underscored.begin(), underscored.end(), '-', '_');
  for (const auto* bag : {"", "properties.", "private-properties."}) {
    paths.push_back("pipeline.hmplaycropper." + std::string(bag) + underscored);
    paths.push_back("pipeline.hmplaycropper." + std::string(bag) + native);
  }
  return Select(config, paths);
}
YAML::Node BaselineOnNull(
    const config::ProgramConfiguration& config,
    const YAML::Node& value,
    const std::string& canonical) {
  if (!value.IsNull())
    return value;
  const auto baseline = config.layers.empty() ? std::nullopt : get_node(config.layers.front(), canonical);
  if (!baseline || !baseline->IsScalar())
    throw std::invalid_argument("Scoreboard reset requires its baseline value: " + canonical);
  return *baseline;
}
bool Flag(const YAML::Node& node) {
  if (node.IsScalar() && (node.Scalar() == "0" || node.Scalar() == "1"))
    return node.Scalar() == "1";
  return node.as<bool>();
}
double Number(const std::string& value) {
  size_t parsed = 0;
  const double number = std::stod(value, &parsed);
  if (parsed != value.size() || !std::isfinite(number))
    throw std::invalid_argument("Scoreboard numeric settings must be finite");
  return number;
}
int Dimension(const YAML::Node& node, int extent, double scale) {
  const auto value = node.as<std::string>();
  if (value.empty())
    throw std::invalid_argument("Scoreboard dimension cannot be empty");
  const bool percent = value[0] == '%';
  const double pixels = Number(percent ? value.substr(1) : value) * (percent ? extent / 100.0 : 1.0);
  // Match native truncation before applying scale, while bounding every cast.
  if (pixels < 1 || pixels > INT32_MAX)
    throw std::invalid_argument("Scoreboard dimension exceeds integer pixels");
  const double scaled = std::trunc(pixels) * scale;
  if (scaled < 2 || scaled > extent)
    throw std::invalid_argument("Scoreboard dimension must fit within the output");
  return static_cast<int>(scaled);
}
} // namespace
absl::StatusOr<std::optional<ProgramScoreboardConfig>> ResolveProgramScoreboardConfig(
    const config::ProgramConfiguration& config,
    int output_width,
    int output_height) {
  try {
    if (!Flag(CropSetting(config, {}, "show-scoreboard")))
      return std::nullopt;
    if (output_width < 2 || output_height < 2)
      return absl::InvalidArgumentError("Scoreboard requires positive output dimensions");
    const auto polygon = CropSetting(config, "rink.scoreboard.perspective_polygon", "scoreboard-perspective-polygon");
    if (polygon.IsNull())
      return absl::FailedPreconditionError("AMD scoreboard requires a saved polygon; select it before playback");
    ProgramScoreboardConfig result;
    if (polygon.IsSequence() && polygon.size() == 4) {
      for (size_t i = 0; i < 4; ++i) {
        if (!polygon[i].IsSequence() || polygon[i].size() != 2)
          return absl::InvalidArgumentError("Scoreboard polygon requires four x,y points");
        for (size_t j = 0; j < 2; ++j)
          result.polygon[i][j] = polygon[i][j].as<float>();
      }
    } else if (polygon.IsScalar()) {
      std::istringstream values(polygon.Scalar());
      std::string token;
      for (size_t i = 0; i < 8; ++i) {
        if (!std::getline(values, token, ','))
          return absl::InvalidArgumentError("Scoreboard native polygon requires eight coordinates");
        result.polygon[i / 2][i % 2] = Number(token);
      }
      if (std::getline(values, token, ',') || polygon.Scalar().back() == ',')
        return absl::InvalidArgumentError("Scoreboard native polygon has extra coordinates");
    } else {
      return absl::InvalidArgumentError("Scoreboard polygon requires four x,y points");
    }
    bool disabled = true;
    for (const auto& point : result.polygon)
      for (const auto number : point) {
        if (!std::isfinite(number))
          return absl::InvalidArgumentError("Scoreboard polygon must be finite");
        disabled = disabled && number == 0;
      }
    if (disabled)
      return std::nullopt;
    const auto generation = get_node(config.values, "rink.stitched_output_generation");
    if (!generation || !generation->IsScalar() || generation->Scalar().empty())
      return absl::FailedPreconditionError("Scoreboard requires a saved output-generation identity");
    result.generation = generation->Scalar();
    const double scale = Number(BaselineOnNull(
                                    config,
                                    CropSetting(config, "rink.scoreboard.scoreboard_scale", "scoreboard-scale"),
                                    "rink.scoreboard.scoreboard_scale")
                                    .as<std::string>());
    if (scale <= 0)
      return absl::InvalidArgumentError("Scoreboard scale must be positive");
    result.requested_width = Dimension(
        BaselineOnNull(
            config,
            CropSetting(config, "rink.scoreboard.projected_width", "scoreboard-projected-width"),
            "rink.scoreboard.projected_width"),
        output_width,
        scale);
    result.requested_height = Dimension(
        BaselineOnNull(
            config,
            CropSetting(config, "rink.scoreboard.projected_height", "scoreboard-projected-height"),
            "rink.scoreboard.projected_height"),
        output_height,
        scale);
    const auto position = BaselineOnNull(
        config,
        Select(config, {std::begin(scoreboard::kPositionPaths), std::end(scoreboard::kPositionPaths)}),
        "rink.scoreboard.position");
    const auto parsed = position.IsScalar() ? scoreboard::ParsePosition(position.Scalar()) : std::nullopt;
    if (!parsed)
      return absl::InvalidArgumentError("Scoreboard position requires upper-left, upper-center or upper-right");
    result.position = *parsed;
    const auto filter = get_node(config.values, "rink.scoreboard.resize_filter");
    const auto sharpen = get_node(config.values, "rink.scoreboard.sharpen_amount");
    if (!filter || !sharpen)
      return absl::InvalidArgumentError("Scoreboard filter settings must come from the baseline");
    if (filter->as<std::string>() != "bilinear" || Number(sharpen->as<std::string>()) != 0)
      return absl::UnimplementedError("AMD scoreboard currently requires bilinear sampling without sharpening");
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid Program scoreboard configuration: ") + error.what());
  }
}
absl::StatusOr<scoreboard::Plan> PrepareProgramScoreboard(
    const ProgramScoreboardConfig& config,
    const std::string& generation,
    int source_width,
    int source_height,
    int output_width,
    int output_height) {
  if (generation.empty() || generation != config.generation)
    return absl::FailedPreconditionError("Scoreboard polygon belongs to a different output generation");
  return scoreboard::MakePlan(
      config.polygon,
      source_width,
      source_height,
      output_width,
      output_height,
      config.requested_width,
      config.requested_height,
      config.position);
}
} // namespace hm::program
