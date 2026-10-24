#include "hstream/src/libs/program/ProgramAudioConfig.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::program {
namespace {
int Rank(const config::ProgramConfiguration& config, const std::string& path) {
  const auto it = config.ranks.find(path);
  return it == config.ranks.end() ? 0 : it->second;
}
YAML::Node Choice(const config::ProgramConfiguration& config, const std::string& block, const char* field) {
  YAML::Node result;
  int rank = -1;
  for (const auto& name : {block, std::string("hmaudio")}) {
    auto underscore = std::string(field);
    std::replace(underscore.begin(), underscore.end(), '-', '_');
    for (const auto& key : {underscore, std::string(field)}) {
      const auto path = "pipeline." + name + "." + key;
      const auto value = get_node(config.values, path);
      if (value && !value->IsNull() && Rank(config, path) >= rank) {
        result.reset(YAML::Clone(*value));
        rank = Rank(config, path);
      }
    }
  }
  return result;
}
bool Flag(const YAML::Node& node) {
  if (node.IsScalar() && (node.Scalar() == "0" || node.Scalar() == "1"))
    return node.Scalar() == "1";
  return node.as<bool>();
}
int64_t Positive(const config::ProgramConfiguration& config, const std::string& path, int64_t maximum) {
  const auto node = get_node(config.values, path);
  if (!node || !node->IsScalar())
    throw std::invalid_argument("Missing audio bound: " + path);
  const auto value = node->as<int64_t>();
  if (value <= 0 || value > maximum)
    throw std::invalid_argument("Audio bound is outside its supported range: " + path);
  return value;
}
} // namespace
absl::StatusOr<std::optional<ProgramAudioConfig>> ResolveProgramAudioConfig(
    const config::ProgramConfiguration& config,
    const std::array<double, 2>& offsets) {
  try {
    std::vector<std::string> blocks;
    for (const auto& entry : config.values["pipeline"]) {
      const auto name = entry.first.as<std::string>();
      if (name.rfind("hmaudio", 0) == 0 && name != "hmaudio") {
        if (!std::all_of(name.begin() + 7, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
          return absl::InvalidArgumentError("Invalid audio block name: " + name);
        blocks.push_back(name);
      }
    }
    if (blocks.empty() && get_node(config.values, "pipeline.hmaudio"))
      blocks.push_back("hmaudio");
    std::optional<ProgramAudioConfig> output;
    for (const auto& block : blocks) {
      const auto enabled = Choice(config, block, "enable");
      if (enabled.IsNull() || !Flag(enabled))
        continue;
      if (output)
        return absl::UnimplementedError("AMD Program requires one enabled source-linked audio block");
      if (Choice(config, block, "src").as<int>() != 2 || Choice(config, block, "dest").as<int>() != 1 ||
          Choice(config, block, "sink-id").as<int>() != -1)
        return absl::UnimplementedError("AMD Program audio requires src=2, dest=1 and sink-id=-1");
      if ((!std::isfinite(offsets[0]) || offsets[0] < 0) || (!std::isfinite(offsets[1]) || offsets[1] < 0) ||
          (offsets[0] != 0 && offsets[1] != 0))
        return absl::InvalidArgumentError("Program audio requires a camera with zero stitching offset");
      const auto selected = Choice(config, block, "source-id");
      const int camera = selected.IsNull() ? (offsets[0] == 0 ? 0 : 1) : selected.as<int>();
      if (camera < 0 || camera > 1 || offsets[camera] != 0)
        return absl::InvalidArgumentError("Program audio must select a camera with zero stitching offset");
      ProgramAudioConfig result{};
      result.camera = camera;
      result.reader.operation_timeout_ns =
          Positive(config, "runtime.amd.operation_timeout_ms", INT64_MAX / 1000000) * 1000000;
      result.reader.maximum_packet_bytes = Positive(config, "runtime.amd.audio.maximum_packet_bytes", INT_MAX);
      result.reader.maximum_frame_samples =
          Positive(config, "runtime.amd.audio.maximum_frame_samples", INT_MAX / (2 * sizeof(float)));
      result.queue_samples = Positive(config, "runtime.amd.audio.queue_samples", INT_MAX / (2 * sizeof(float)));
      result.bitrate = Positive(config, "runtime.amd.audio.bitrate", INT_MAX);
      if (result.queue_samples < result.reader.maximum_frame_samples)
        return absl::InvalidArgumentError("Program audio queue must hold a complete bounded PCM frame");
      const auto device = Choice(config, block, "alsa-dest-device");
      if (!device.IsNull())
        result.alsa_device = device.as<std::string>();
      output = std::move(result);
    }
    return output;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid Program audio configuration: ") + error.what());
  }
}
} // namespace hm::program
