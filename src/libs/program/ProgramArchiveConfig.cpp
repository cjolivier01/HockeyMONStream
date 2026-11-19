#include "hstream/src/libs/program/ProgramArchiveConfig.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <iostream>
#include <stdexcept>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/KeyframeInterval.h"

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
    const std::optional<BitratePerPixel>& source_bitrate,
    const std::optional<std::pair<int, int>>& source_frame_rate) {
  try {
    std::map<std::string, ProgramArchiveConfig> result;
    std::set<std::filesystem::path> paths;
    std::set<int> ids;
    for (const auto& name : sinks) {
      const bool four_k = name == "ENCODE_PROGRAM_4K_FILE";
      const bool mobile = name == "ENCODE_PROGRAM_MOBILE_FILE";
      // Both derived copies take their geometry from Program rather than from
      // per-sink width/height, and neither inherits the camera's bitrate.
      const bool derived = four_k || mobile;
      if (!derived && name != "ENCODE_FILE")
        continue;
      const int sink_type = four_k ? 9 : mobile ? 10 : 3;
      YAML::Node sink;
      std::string prefix, section;
      for (const auto& entry : config.values["pipeline"]) {
        const auto key = entry.first.as<std::string>();
        const auto node = entry.second;
        if (key.rfind("sink", 0) != 0 || !node.IsMap() || !node["type"] || node["type"].as<int>() != sink_type)
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
      if (derived) {
        // 4K names the UHD bound, as in the shared NVIDIA archive sink: a
        // format contract, not another configurable output-size default. The
        // mobile copy is the opposite — its ceiling is the whole point of the
        // route, so it fails closed rather than inventing one here.
        double max_width = 3840.0, max_height = 2160.0;
        std::string canonical = "video_out.program_4k_interpolation";
        if (mobile) {
          const auto settings = config.values["video_out"]["program_mobile"];
          if (!settings.IsMap())
            return absl::InvalidArgumentError("Effective baseline is missing supported key video_out.program_mobile");
          // Honour the same sink-local overrides NVIDIA accepts, with the same
          // precedence, or the two backends resolve different ceilings from one
          // configuration.
          const std::pair<const char*, const char*> bounds[] = {
              {"max_width", "max-output-width"},
              {"max_height", "max-output-height"},
          };
          for (const auto& [canonical_leaf, native] : bounds) {
            const std::string canonical_path = std::string("video_out.program_mobile.") + canonical_leaf;
            YAML::Node value = settings[canonical_leaf];
            if (!Null(sink[native]) && Rank(config, prefix + native) >= Rank(config, canonical_path))
              value.reset(YAML::Clone(sink[native]));
            const auto resolved = double(Positive(value, canonical_path));
            (std::string(canonical_leaf) == "max_width" ? max_width : max_height) = resolved;
          }
          canonical = "video_out.program_mobile.interpolation";
        }
        const double scale = std::min({1.0, max_width / output.width, max_height / output.height});
        output.width = int(output.width * scale) & ~1;
        output.height = int(output.height * scale) & ~1;
        auto interpolation = mobile ? config.values["video_out"]["program_mobile"]["interpolation"]
                                    : config.values["video_out"]["program_4k_interpolation"];
        if (sink["interpolation-method"] && Rank(config, prefix + "interpolation-method") >= Rank(config, canonical))
          interpolation.reset(YAML::Clone(sink["interpolation-method"]));
        if (!interpolation.IsScalar())
          return absl::InvalidArgumentError(canonical + " must name a scaling filter");
        // Unlike the NVIDIA sink, this branch renders the crop straight into a
        // UHD surface instead of resizing a full-size Program, so there is no
        // second pass for the filter to name. RenderProgramYuv already area
        // averages the footprints it reduces far enough to need it, so report
        // the unused setting rather than failing on a value the baseline ships.
        std::cerr << "AMD Program " << (mobile ? "mobile" : "4K") << " does not select a resize filter through "
                  << canonical << '=' << interpolation.Scalar()
                  << "; its crop area averages any footprint it reduces by 1.75x or more, up to 4 taps per axis\n";
      }
      // Native per-sink sizes are scaling choices. Keep the same camera aspect
      // as Program; rendering a different aspect would change its field of view.
      for (const auto& dimension : {std::string("width"), std::string("height")}) {
        const auto node = sink[dimension];
        if (Null(node))
          continue;
        if (derived)
          return absl::UnimplementedError(
              std::string("Program ") + (four_k ? "4K" : "mobile") + " derives its size from the Program aspect");
        const auto canonical = "video_out.output_" + dimension;
        if (Rank(config, prefix + dimension) >= Rank(config, canonical) &&
            Rank(config, prefix + dimension) >= Rank(config, "runtime.amd.output." + dimension))
          (dimension == "width" ? output.width : output.height) = Positive(node, prefix + dimension);
      }
      if (output.width < 2 || output.height < 2 || ((output.width | output.height) & 1))
        return absl::InvalidArgumentError("AMD archive dimensions must be positive even integers");
      if (!derived && int64_t(output.width) * program.height != int64_t(output.height) * program.width)
        return absl::UnimplementedError("AMD archive scaling must preserve the Program aspect");

      const auto canonical_bitrate = config.values["video_out"]["bit_rate"];
      YAML::Node bitrate = sink["bitrate"];
      int bitrate_rank = Rank(config, prefix + "bitrate");
      const int amd_bitrate_rank = Rank(config, "runtime.amd.output.bitrate_kbps");
      if (mobile) {
        // This route exists so a phone does not receive the camera's
        // acquisition rate, so it never reaches the source-bits-per-pixel
        // branch below. Only a higher-ranked explicit per-sink bitrate wins.
        // A direct native property wins over its canonical mapping at the same
        // layer, structural app YAML included, so a defined sink-local bitrate
        // is only displaced by a canonical value from a higher one. Requiring
        // an explicit rank here would discard a structural override NVIDIA
        // honours and quietly resolve a different bitrate per backend.
        const std::string path = "video_out.program_mobile.bitrate";
        if (Null(bitrate) || bitrate_rank < Rank(config, path))
          bitrate.reset(YAML::Clone(config.values["video_out"]["program_mobile"]["bitrate"]));
        output.bitrate_kbps = (Positive(bitrate, path) + 999) / 1000;
        // Keyframes are configured in seconds and set on the encoder in frames.
        // A sink-local interval is already in frames and wins at the same or a
        // higher explicit layer. Pick the winner before validating anything, as
        // the NVIDIA branch does, so a losing canonical value cannot fail the
        // run and the two backends accept the same configurations.
        const std::string seconds_path = "video_out.program_mobile.keyframe_interval_seconds";
        const int native_interval_rank = Rank(config, prefix + "iframeinterval");
        if (!Null(sink["iframeinterval"]) && native_interval_rank >= 1 &&
            native_interval_rank >= Rank(config, seconds_path)) {
          output.keyframe_interval = unsigned(Positive(sink["iframeinterval"], prefix + "iframeinterval"));
        } else {
          if (!source_frame_rate || source_frame_rate->first <= 0 || source_frame_rate->second <= 0)
            return absl::InvalidArgumentError(
                "The mobile Program archive needs the source frame rate to resolve " + seconds_path);
          const auto seconds = config.values["video_out"]["program_mobile"]["keyframe_interval_seconds"];
          if (Null(seconds) || !seconds.IsScalar())
            return absl::InvalidArgumentError("Effective baseline is missing supported key " + seconds_path);
          const double value = seconds.as<double>();
          if (!(value > 0.0) || !std::isfinite(value))
            return absl::InvalidArgumentError(seconds_path + " must be a positive number of seconds");
          const auto frames = keyframe_interval_frames(value, source_frame_rate->first, source_frame_rate->second);
          if (!frames)
            return absl::InvalidArgumentError(seconds_path + " is not a usable keyframe interval at the source rate");
          output.keyframe_interval = *frames;
        }
      } else {
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
      // video_out.output_video_path names the Program video, not the copies
      // derived from it. Applying it to a derived route would resolve two
      // archives to one filename and fail the duplicate-path preflight.
      if (!derived && Rank(config, "video_out.output_video_path") > Rank(config, prefix + "output-file")) {
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
      result.emplace(
          name, ProgramArchiveConfig{id, four_k ? "program-4k" : mobile ? "program-mobile" : "program", path, output});
    }
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid Program archive configuration: ") + error.what());
  }
}
} // namespace hm::program
