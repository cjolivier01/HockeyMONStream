#include "hstream/src/libs/common/RuntimeControlConfig.h"

#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::config {
namespace {
void Set(YAML::Node node, const std::string& path, const YAML::Node& value, bool remove = false) {
  const auto dot = path.find('.');
  if (dot == std::string::npos) {
    if (remove)
      node.remove(path);
    else
      node[path] = YAML::Clone(value);
    return;
  }
  const auto key = path.substr(0, dot);
  if (remove && !node[key].IsMap())
    return;
  if (!remove && node[key].IsDefined() && !node[key].IsNull() && !node[key].IsMap())
    throw std::invalid_argument("Runtime configuration requires a map at " + key);
  Set(node[key], path.substr(dot + 1), value, remove);
  if (remove && node[key].IsMap() && node[key].size() == 0)
    node.remove(key);
}
bool Boolean(const YAML::Node& value) {
  if (value.IsScalar() && (value.Scalar() == "0" || value.Scalar() == "1"))
    return value.Scalar() == "1";
  return value.as<bool>();
}
double Number(const YAML::Node& value) {
  const double result = value.as<double>();
  if (!std::isfinite(result))
    throw std::invalid_argument("Runtime control must be finite");
  return result;
}
const RuntimeControlKey& Key(const std::string& id) {
  for (const auto& key : kRuntimeControlKeys)
    if (id == key.id)
      return key;
  throw std::invalid_argument("Unknown runtime control " + id);
}
double Seconds(const YAML::Node& value) {
  const auto text = value.as<std::string>();
  double result = 0;
  size_t begin = 0;
  int parts = 0;
  while (true) {
    const auto end = text.find(':', begin);
    const auto part = text.substr(begin, end == std::string::npos ? end : end - begin);
    size_t consumed = 0;
    const double number = std::stod(part, &consumed);
    if (consumed != part.size() || !std::isfinite(number) || number < 0 || ++parts > 3 || (parts > 1 && number >= 60))
      throw std::invalid_argument("Invalid playback start time " + text);
    result = result * 60 + number;
    if (end == std::string::npos)
      return result;
    if (std::floor(number) != number)
      throw std::invalid_argument("Only playback seconds may be fractional");
    begin = end + 1;
  }
}
} // namespace

namespace {
void Normalize(const YAML::Node& layer, YAML::Node result) {
  if (!result.IsMap())
    return;
  const auto move = [&](const std::string& legacy, const std::string& canonical, auto convert) {
    const auto value = get_node(layer, legacy);
    if (!value)
      return;
    if (!get_node(layer, canonical))
      Set(result, canonical, convert(*value));
    Set(result, legacy, YAML::Node(), true);
  };
  // AMD fallbacks first; legacy shared UI tone won same-layer ties.
  for (const auto* key : {"shadow_lift_percent", "lift_shadow_black_point", "exposure"})
    move(std::string("runtime.amd.output.") + key, std::string("video_out.tone.") + key, [](const YAML::Node& value) {
      return YAML::Clone(value);
    });
  for (const auto& key : kRuntimeControlKeys)
    move(std::string("hstream_ui.camera_controls.") + key.id, key.path, [&](const YAML::Node& value) {
      return key.boolean ? YAML::Node(Boolean(value)) : YAML::Node(Number(value) / key.divisor);
    });
  move("runtime.amd.playback_start_seconds", "playback.start_seconds", [](const YAML::Node& value) {
    return YAML::Node(Number(value));
  });
  move("hstream_ui.playback_start_time", "playback.start_seconds", [](const YAML::Node& value) {
    return YAML::Node(Seconds(value));
  });
  move("runtime.amd.output.realtime", "playback.realtime", [](const YAML::Node& value) {
    return YAML::Node(Boolean(value));
  });
  move("hstream_ui.playback_realtime", "playback.realtime", [](const YAML::Node& value) {
    return YAML::Node(Boolean(value));
  });
  move("hstream_ui.drivegpt_database.enabled", "telemetry.enabled", [](const YAML::Node& value) {
    return YAML::Node(Boolean(value));
  });
}
} // namespace

YAML::Node NormalizeRuntimeControlLayer(const YAML::Node& layer) {
  auto result = YAML::Clone(layer);
  Normalize(layer, result);
  return result;
}

void NormalizeRuntimeControlLayerInPlace(YAML::Node layer) {
  Normalize(YAML::Clone(layer), layer);
}

int64_t PlaybackStartNanoseconds(const YAML::Node& config) {
  const auto layer = NormalizeRuntimeControlLayer(config);
  const auto value = get_node(layer, "playback.start_seconds");
  if (!value)
    throw std::invalid_argument("Missing playback.start_seconds");
  const double seconds = Number(*value);
  const long double ns = static_cast<long double>(seconds) * 1'000'000'000;
  if (seconds < 0 || ns > std::numeric_limits<int64_t>::max())
    throw std::invalid_argument("playback.start_seconds must fit nonnegative nanoseconds");
  return static_cast<int64_t>(std::round(ns));
}

std::string PlaybackStartTime(const YAML::Node& config) {
  const auto ms = PlaybackStartNanoseconds(config) / 1'000'000;
  if (ms >= 86'400'000)
    throw std::invalid_argument("playback.start_seconds must be within one day for time controls");
  std::ostringstream text;
  text << std::setfill('0') << std::setw(2) << ms / 3'600'000 << ':' << std::setw(2) << ms / 60'000 % 60 << ':'
       << std::setw(2) << ms / 1000 % 60;
  if (ms % 1000)
    text << '.' << std::setw(3) << ms % 1000;
  return text.str();
}

double RuntimeControlPresentationValue(const YAML::Node& config, const std::string& id) {
  const auto& key = Key(id);
  const auto value = get_node(config, key.path);
  if (!value)
    throw std::invalid_argument(std::string("Missing runtime control ") + key.path);
  return key.boolean ? Boolean(*value) : Number(*value) * key.divisor;
}

} // namespace hm::config
