#include "hstream/src/libs/program/ProgramArchiveConfig.h"

#include <algorithm>
#include <climits>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"

namespace hm::program {
namespace {
int Rank(const config::ProgramConfiguration& config, const std::string& path) {
  const auto value = config.ranks.find(path);
  return value == config.ranks.end() ? 0 : value->second;
}
bool Null(const YAML::Node& node) {
  return !node.IsDefined() || node.IsNull() || (node.IsScalar() && (node.Scalar() == "null" || node.Scalar() == "~"));
}
uint64_t Positive(const YAML::Node& node, const std::string& path) {
  const auto number = node.as<int64_t>();
  if (number <= 0 || number > INT_MAX)
    throw std::invalid_argument("Archive setting must fit a positive native integer: " + path);
  return number;
}
} // namespace
absl::StatusOr<std::map<std::string, ProgramArchiveConfig>> ResolveProgramArchives(
    const config::ProgramConfiguration& config,
    const std::set<std::string>& sinks,
    const ProgramOutputConfig& program,
    const std::filesystem::path& directory,
    const std::optional<BitratePerPixel>& source_bitrate) {
  try {
    std::map<std::string, ProgramArchiveConfig> result;
    std::set<std::filesystem::path> paths;
    std::set<int> ids;
    for (const auto& name : sinks) {
      const bool four_k = name == "ENCODE_PROGRAM_4K_FILE";
      if (!four_k && name != "ENCODE_FILE")
        continue;
      YAML::Node sink;
      std::string prefix, section;
      for (const auto& entry : config.values["pipeline"]) {
        const auto key = entry.first.as<std::string>();
        const auto node = entry.second;
        if (key.rfind("sink", 0) != 0 || !node.IsMap() || !node["type"] || node["type"].as<int>() != (four_k ? 9 : 3))
          continue;
        if (!prefix.empty())
          return absl::InvalidArgumentError("AMD Program requires one configured sink per archive kind: " + name);
        sink.reset(YAML::Clone(node));
        section = key;
        prefix = "pipeline." + key + ".";
      }
      if (prefix.empty())
        return absl::InvalidArgumentError("Missing archive sink definition: " + name);
      const int id = sink["sink-id"].as<int>();
      if (id < 0 || sink["codec"].as<int>() != 2 || sink["enc-type"].as<int>() != 0)
        return absl::UnimplementedError("AMD archives require a nonnegative sink ID and hardware HEVC");
      if (!ids.insert(id).second)
        return absl::InvalidArgumentError("Program archive sink IDs must be unique");
      if ((Rank(config, prefix + "container") > 0 && sink["container"].as<int>() != 1) ||
          (Rank(config, prefix + "profile") > 0 && sink["profile"].as<int>() != 1))
        return absl::UnimplementedError("AMD archives currently require Main10 HEVC in MP4");

      auto output = program;
      // 4K names the UHD bound, as in the shared NVIDIA archive sink. This is
      // a format contract, not another configurable output-size default.
      if (four_k) {
        const double scale = std::min({1.0, 3840.0 / output.width, 2160.0 / output.height});
        output.width = int(output.width * scale) & ~1;
        output.height = int(output.height * scale) & ~1;
        const std::string canonical = "video_out.program_4k_interpolation";
        auto interpolation = config.values["video_out"]["program_4k_interpolation"];
        if (sink["interpolation-method"] && Rank(config, prefix + "interpolation-method") >= Rank(config, canonical))
          interpolation.reset(YAML::Clone(sink["interpolation-method"]));
        if (!interpolation.IsScalar())
          return absl::InvalidArgumentError(canonical + " must name a scaling filter");
        // Unlike the NVIDIA sink, this branch renders the crop straight into a
        // UHD surface instead of resizing a full-size Program, so there is no
        // second pass for the filter to name. RenderProgramYuv already area
        // averages the footprints it reduces far enough to need it, so report
        // the unused setting rather than failing on a value the baseline ships.
        std::cerr << "AMD Program 4K does not select a resize filter through " << canonical << '='
                  << interpolation.Scalar()
                  << "; its crop area averages any footprint it reduces by 1.75x or more, up to 4 taps per axis\n";
      }
      // Native per-sink sizes are scaling choices. Keep the same camera aspect
      // as Program; rendering a different aspect would change its field of view.
      for (const auto& dimension : {std::string("width"), std::string("height")}) {
        const auto node = sink[dimension];
        if (Null(node))
          continue;
        if (four_k)
          return absl::UnimplementedError("Program 4K derives its size from the Program aspect");
        const auto canonical = "video_out.output_" + dimension;
        if (Rank(config, prefix + dimension) >= Rank(config, canonical) &&
            Rank(config, prefix + dimension) >= Rank(config, "runtime.amd.output." + dimension))
          (dimension == "width" ? output.width : output.height) = Positive(node, prefix + dimension);
      }
      if (output.width < 2 || output.height < 2 || ((output.width | output.height) & 1))
        return absl::InvalidArgumentError("AMD archive dimensions must be positive even integers");
      if (!four_k && int64_t(output.width) * program.height != int64_t(output.height) * program.width)
        return absl::UnimplementedError("AMD archive scaling must preserve the Program aspect");

      const auto canonical_bitrate = config.values["video_out"]["bit_rate"];
      YAML::Node bitrate = sink["bitrate"];
      int bitrate_rank = Rank(config, prefix + "bitrate");
      const int amd_bitrate_rank = Rank(config, "runtime.amd.output.bitrate_kbps");
      if (!four_k && Rank(config, "video_out.bit_rate") > bitrate_rank &&
          Rank(config, "video_out.bit_rate") > amd_bitrate_rank) {
        bitrate.reset(YAML::Clone(canonical_bitrate));
        bitrate_rank = Rank(config, "video_out.bit_rate");
      }
      if (!Null(bitrate) && bitrate_rank > 0 && bitrate_rank >= amd_bitrate_rank) {
        output.bitrate_kbps = (Positive(bitrate, prefix + "bitrate") + 999) / 1000;
      } else if (amd_bitrate_rank > 0) {
        // The base Program may contain a higher-ranked canonical override,
        // which applies only to ENCODE_FILE, not the independent 4K branch.
        const std::string path = "runtime.amd.output.bitrate_kbps";
        output.bitrate_kbps = Positive(config.values["runtime"]["amd"]["output"]["bitrate_kbps"], path);
      } else if (source_bitrate) {
        const auto bits = scale_bitrate(*source_bitrate, output.width, output.height);
        if (!bits || !*bits || *bits > INT_MAX)
          return absl::InvalidArgumentError("Derived archive bitrate exceeds the native encoder integer range");
        output.bitrate_kbps = (*bits + 999) / 1000;
      } else if (!Null(bitrate)) {
        output.bitrate_kbps = (Positive(bitrate, prefix + "bitrate") + 999) / 1000;
      }

      YAML::Node file = sink["output-file"];
      bool explicit_file = Rank(config, prefix + "output-file") > 0;
      bool canonical_file = false;
      const auto reset_file = [&] {
        const auto value = config.layers.empty()
            ? std::nullopt
            : get_node(config.layers.front(), "pipeline." + section + ".output-file");
        file.reset(value ? YAML::Clone(*value) : YAML::Node());
        explicit_file = canonical_file = false;
      };
      if (!four_k && Rank(config, "video_out.output_video_path") > Rank(config, prefix + "output-file")) {
        const auto canonical = config.values["video_out"]["output_video_path"];
        if (!Null(canonical)) {
          file.reset(YAML::Clone(canonical));
          explicit_file = canonical_file = true;
        } else
          reset_file();
      }
      if (Null(file))
        reset_file();
      if (Null(file) || !file.IsScalar() || file.Scalar().empty())
        return absl::InvalidArgumentError("Archive output requires a configured filename");
      if (file.Scalar().find_first_of("\r\n") != std::string::npos)
        return absl::InvalidArgumentError("Archive filename cannot contain a line break");
      std::filesystem::path path(file.Scalar());
      if (!explicit_file)
        path.replace_extension(".mp4");
      if (path.extension() != ".mp4")
        return absl::UnimplementedError("AMD archive output requires an .mp4 filename");
      if (!path.has_parent_path() && !canonical_file)
        path = directory / path;
      path = std::filesystem::absolute(path).lexically_normal();
      if (!paths.insert(std::filesystem::weakly_canonical(path)).second)
        return absl::InvalidArgumentError("Program archive outputs resolve to the same file");
      result.emplace(name, ProgramArchiveConfig{id, four_k ? "program-4k" : "program", path, output});
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid Program archive configuration: ") + error.what());
  }
}
} // namespace hm::program
