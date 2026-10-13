#include "hstream/src/libs/stitching/InputLens.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <regex>
#include <set>
#include <sstream>

namespace hm::stitching {

absl::Status ValidateStitchInputLens(const StitchInputLens& lens) {
  if (lens.projection != "rectilinear" && lens.projection != "full-frame-fisheye")
    return absl::InvalidArgumentError("Input lens projection must be rectilinear or full-frame-fisheye");
  if (lens.optimization != "auto" && lens.optimization != "refine" && lens.optimization != "fixed")
    return absl::InvalidArgumentError("Input lens optimization must be auto, refine or fixed");
  for (double value : lens.distortion)
    if (!std::isfinite(value) || std::abs(value) > 1)
      return absl::InvalidArgumentError("Input lens distortion coefficients must be finite and within [-1, 1]");
  for (double value : lens.center_offset)
    if (!std::isfinite(value) || std::abs(value) > 0.5)
      return absl::InvalidArgumentError("Input lens center offsets must be finite fractions within [-0.5, 0.5]");
  return absl::OkStatus();
}

absl::StatusOr<StitchInputLens> ReadStitchInputLens(const YAML::Node& node, const StitchInputLens& defaults) {
  StitchInputLens lens = defaults;
  try {
    if (!node || node.IsNull())
      return lens;
    if (!node.IsMap())
      return absl::InvalidArgumentError("stitching.input_lens must be a map");
    const std::set<std::string> keys{"projection", "optimization", "distortion", "center_offset"};
    for (const auto& entry : node)
      if (!entry.first.IsScalar() || !keys.count(entry.first.as<std::string>()))
        return absl::InvalidArgumentError("Unsupported stitching.input_lens key");
    if (node["projection"] && !node["projection"].IsNull())
      lens.projection = node["projection"].as<std::string>();
    if (node["optimization"] && !node["optimization"].IsNull())
      lens.optimization = node["optimization"].as<std::string>();
    const auto read_array = [&](const char* key, auto& values) -> absl::Status {
      const YAML::Node value = node[key];
      if (!value || value.IsNull())
        return absl::OkStatus();
      if (!value.IsSequence() || value.size() != values.size())
        return absl::InvalidArgumentError(std::string("Invalid stitching.input_lens.") + key + " array size");
      for (size_t i = 0; i < values.size(); ++i)
        values[i] = value[i].as<double>();
      return absl::OkStatus();
    };
    if (const auto status = read_array("distortion", lens.distortion); !status.ok())
      return status;
    if (const auto status = read_array("center_offset", lens.center_offset); !status.ok())
      return status;
    if (const auto status = ValidateStitchInputLens(lens); !status.ok())
      return status;
    return lens;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid stitching.input_lens: ") + error.what());
  }
}

YAML::Node WriteStitchInputLens(const StitchInputLens& lens) {
  YAML::Node node;
  node["projection"] = lens.projection;
  node["optimization"] = lens.optimization;
  for (double value : lens.distortion)
    node["distortion"].push_back(value);
  for (double value : lens.center_offset)
    node["center_offset"].push_back(value);
  return node;
}

std::string SerializeStitchInputLens(const StitchInputLens& lens) {
  YAML::Emitter output;
  output.SetDoublePrecision(17);
  output << YAML::Flow << WriteStitchInputLens(lens);
  return output.c_str();
}

absl::StatusOr<StitchInputLens> ParseStitchInputLens(const std::string& text) {
  try {
    const auto node = YAML::Load(text);
    if (!node.IsMap())
      return absl::InvalidArgumentError("Serialized input lens must be a map");
    return ReadStitchInputLens(node);
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

int StitchInputLensProjection(const StitchInputLens& lens) {
  return lens.projection == "full-frame-fisheye" ? 3 : 0;
}

absl::StatusOr<std::string> ApplyStitchInputLens(const std::string& pto, const StitchInputLens& lens) {
  if (const auto status = ValidateStitchInputLens(lens); !status.ok())
    return status;
  if (lens == StitchInputLens{})
    return pto; // Preserve the legacy automatic alignment exactly.
  std::istringstream input(pto);
  std::ostringstream output;
  output.imbue(std::locale::classic());
  size_t images = 0;
  double first_width = 0, first_height = 0;
  for (std::string line; std::getline(input, line);) {
    // pto_gen enables Hugin's position-only preset. On project load that
    // preset regenerates the variable list and discards our explicit v lines.
    if (lens.optimization != "auto" && line.rfind("#hugin_optimizerMasterSwitch", 0) == 0)
      continue;
    if (line.rfind("v ", 0) == 0 || line == "v") {
      if (lens.optimization != "auto")
        continue;
    }
    if (line.rfind("i ", 0) == 0) {
      std::smatch width, height;
      const std::regex width_pattern(R"(\bw([0-9]+)\b)"), height_pattern(R"(\bh([0-9]+)\b)");
      if (!std::regex_search(line, width, width_pattern) || !std::regex_search(line, height, height_pattern))
        return absl::InvalidArgumentError("Input lens requires source-image dimensions in the PTO");
      double w = 0, h = 0;
      try {
        w = std::stod(width[1]);
        h = std::stod(height[1]);
      } catch (const std::exception&) {
        return absl::InvalidArgumentError("Invalid input lens source-image dimensions");
      }
      if (w <= 0 || h <= 0 || w > 32768 || h > 32768)
        return absl::InvalidArgumentError("Invalid input lens source-image dimensions");
      if (images && (w != first_width || h != first_height))
        return absl::InvalidArgumentError("Shared input lens correction requires equal source-image dimensions");
      first_width = w;
      first_height = h;
      const auto set = [&](const char* key, double value, bool linked) {
        std::ostringstream token;
        token.imbue(std::locale::classic());
        token << ' ' << key;
        if (linked)
          token << "=0";
        else
          token << std::setprecision(17) << value;
        const std::regex pattern(std::string(" ") + key + R"(=?[-+0-9.eE]+(?=\s|$))");
        if (std::regex_search(line, pattern))
          line = std::regex_replace(line, pattern, token.str());
        else
          line += token.str();
      };
      set("f", StitchInputLensProjection(lens), false);
      for (size_t i = 0; i < lens.distortion.size(); ++i)
        set(std::string(1, 'a' + i).c_str(), lens.distortion[i], images != 0);
      set("d", lens.center_offset[0] * w, images != 0);
      set("e", lens.center_offset[1] * h, images != 0);
      ++images;
    }
    output << line << '\n';
  }
  if (images != 2)
    return absl::InvalidArgumentError("Input lens calibration requires two source images");
  if (lens.optimization != "auto") {
    output << "#hugin_optimizerMasterSwitch 0\n";
    output << "v r0 p0 r1 p1 y1";
    if (lens.optimization == "refine")
      output << " v0 b0 d0 e0";
    output << "\nv\n";
  }
  return output.str();
}

} // namespace hm::stitching
