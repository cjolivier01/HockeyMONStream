#include "hstream/src/libs/stitching/InputLens.h"

#include <cmath>
#include <iomanip>
#include <locale>
#include <regex>
#include <set>
#include <sstream>

namespace hm::stitching {

absl::StatusOr<std::optional<StitchSourceSizes>> ReadStitchSourceSizeSuffix(
    std::vector<std::string>& lines,
    bool* display_oriented) {
  if (display_oriented)
    *display_oriented = false;
  for (int version = 22; version <= 29; ++version) {
    if (lines.empty() || lines.front() != "version=" + std::to_string(version))
      continue;
    if (lines.size() < 5)
      return absl::FailedPreconditionError("Missing calibrated source sizes");
    StitchSourceSizes sizes{};
    size_t index = lines.size() - 4;
    for (size_t camera = 0; camera < sizes.size(); ++camera) {
      for (size_t axis = 0; axis < 2; ++axis) {
        const auto key = "source-" + std::to_string(camera) + (axis ? "-height=" : "-width=");
        const auto& line = lines[index++];
        if (line.rfind(key, 0) != 0)
          return absl::FailedPreconditionError("Missing calibrated dimension " + key);
        const std::string value = line.substr(key.size());
        if (value.empty() || value.size() > 5 || value.find_first_not_of("0123456789") != std::string::npos)
          return absl::FailedPreconditionError("Invalid calibrated dimension " + key);
        sizes[camera][axis] = std::stoi(value);
        if (!sizes[camera][axis] || sizes[camera][axis] > 32768)
          return absl::FailedPreconditionError("Invalid calibrated dimension " + key);
      }
    }
    lines.resize(lines.size() - 4);
    if (display_oriented)
      *display_oriented = version >= 26;
    lines.front() = "version=" + std::to_string(version >= 26 ? version - 8 : version - 4);
    return sizes;
  }
  return std::nullopt;
}

absl::Status CheckStitchSourceSizes(const std::optional<StitchSourceSizes>& expected, const StitchSourceSizes& actual) {
  if (!expected && actual[0] != actual[1])
    return absl::FailedPreconditionError(
        "Mixed camera resolutions require verifiable source dimensions. Recalibrate stitching.");
  if (expected) {
    for (size_t i = 0; i < actual.size(); ++i) {
      const auto& calibrated = (*expected)[i];
      if (actual[i] != calibrated)
        return absl::FailedPreconditionError(
            "Camera " + std::to_string(i) + " decoded at " + std::to_string(actual[i][0]) + "x" +
            std::to_string(actual[i][1]) + ", calibrated at " + std::to_string(calibrated[0]) + "x" +
            std::to_string(calibrated[1]));
    }
  }
  return absl::OkStatus();
}

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

absl::Status ValidateStitchCameraOptics(const StitchCameraOptics& camera) {
  if (!std::isfinite(camera.horizontal_fov) || camera.horizontal_fov <= 0 || camera.horizontal_fov >= 360 ||
      !std::isfinite(camera.vertical_fov) || camera.vertical_fov <= 0 || camera.vertical_fov > 180)
    return absl::InvalidArgumentError(
        "Each input camera requires horizontal FOV in (0, 360) and vertical FOV in (0, 180]");
  if (camera.source_size)
    for (int dimension : *camera.source_size)
      if (dimension <= 0 || dimension > 32768)
        return absl::InvalidArgumentError("Invalid input-camera source dimensions");
  if (!camera.configuration.empty() &&
      !std::regex_match(camera.configuration, std::regex("^[a-z0-9]+(?:-[a-z0-9]+)*$")))
    return absl::InvalidArgumentError("Invalid input-camera profile identifier");
  return ValidateStitchInputLens(camera.input_lens);
}

absl::StatusOr<StitchInputCameras> ReadStitchInputCameras(const YAML::Node& node) {
  StitchInputCameras result;
  try {
    if (!node || node.IsNull())
      return result;
    if (!node.IsMap() || node.size() > 2)
      return absl::InvalidArgumentError("stitching.input_cameras supports at most two source cameras");
    for (const auto& entry : node) {
      if (!entry.first.IsScalar() || !entry.second.IsMap())
        return absl::InvalidArgumentError("Input cameras require source paths and complete prescriptions");
      const auto source = entry.first.as<std::string>();
      if (source.empty() || source.size() > 4096 || source.find_first_of("\r\n") != std::string::npos ||
          source.find('\0') != std::string::npos || result.count(source))
        return absl::InvalidArgumentError("Invalid or repeated input-camera source path");
      const auto value = entry.second;
      static const std::set<std::string> keys = {
          "horizontal_fov",
          "vertical_fov",
          "input_lens",
          "source_size",
          "flip_horizontal",
          "flip_vertical",
          "camera_config",
          "camera_automation",
          "auto_input_lens_frozen"};
      for (const auto& item : value)
        if (!item.first.IsScalar() || !keys.count(item.first.as<std::string>()))
          return absl::InvalidArgumentError("Unknown input-camera setting");
      if (!value["horizontal_fov"].IsScalar() || !value["vertical_fov"].IsScalar() || !value["input_lens"].IsMap() ||
          value["input_lens"].size() != 4 || !value["input_lens"]["projection"].IsScalar() ||
          !value["input_lens"]["optimization"].IsScalar() || !value["input_lens"]["distortion"].IsSequence() ||
          !value["input_lens"]["center_offset"].IsSequence())
        return absl::InvalidArgumentError("Input-camera prescriptions must include both FOV axes and all lens values");
      auto lens = ReadStitchInputLens(value["input_lens"]);
      if (!lens.ok())
        return lens.status();
      StitchCameraOptics camera{value["horizontal_fov"].as<double>(), value["vertical_fov"].as<double>(), *lens};
      if (value["flip_horizontal"])
        camera.flip.horizontal = value["flip_horizontal"].as<bool>();
      if (value["flip_vertical"])
        camera.flip.vertical = value["flip_vertical"].as<bool>();
      if (const auto size = value["source_size"]; size.IsDefined()) {
        if (!size.IsSequence() || size.size() != 2)
          return absl::InvalidArgumentError("Input-camera source_size must be [width, height]");
        camera.source_size = std::array<int, 2>{size[0].as<int>(), size[1].as<int>()};
      }
      if (const auto profile = value["camera_config"]; profile.IsDefined()) {
        if (!profile.IsScalar() || profile.as<std::string>().empty())
          return absl::InvalidArgumentError("Input-camera profile must be a nonempty identifier");
        camera.configuration = profile.as<std::string>();
      }
      if (const auto policies = value["camera_automation"]; policies.IsDefined()) {
        if (!policies.IsMap())
          return absl::InvalidArgumentError("Input-camera automation must be a map");
        for (const auto& policy : policies) {
          if (!policy.first.IsScalar() ||
              (policy.first.as<std::string>() != "fov" && policy.first.as<std::string>() != "lens"))
            return absl::InvalidArgumentError("Unknown input-camera automation setting");
          if (!policy.second.IsNull())
            (policy.first.as<std::string>() == "fov" ? camera.auto_fov : camera.auto_lens) = policy.second.as<bool>();
        }
      }
      if (const auto frozen = value["auto_input_lens_frozen"]; frozen.IsDefined())
        camera.frozen = frozen.as<bool>();
      if (const auto status = ValidateStitchCameraOptics(camera); !status.ok())
        return status;
      result.emplace(source, std::move(camera));
    }
    return result;
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid input cameras: ") + error.what());
  }
}

CameraFrameFlips FrameFlips(const StitchInputCameras& cameras) {
  CameraFrameFlips flips;
  for (const auto& [source, camera] : cameras)
    if (camera.flip.enabled())
      flips.emplace(source, camera.flip);
  return flips;
}

absl::StatusOr<CameraFrameFlips> ReadCameraFrameFlips(const YAML::Node& input_cameras) {
  auto cameras = ReadStitchInputCameras(input_cameras);
  if (!cameras.ok())
    return cameras.status();
  return FrameFlips(*cameras);
}

YAML::Node WriteStitchInputCameras(const StitchInputCameras& cameras, bool include_policies) {
  YAML::Node node(YAML::NodeType::Map);
  for (const auto& [source, camera] : cameras) {
    node[source]["horizontal_fov"] = camera.horizontal_fov;
    node[source]["vertical_fov"] = camera.vertical_fov;
    node[source]["input_lens"] = WriteStitchInputLens(camera.input_lens);
    if (camera.flip.horizontal)
      node[source]["flip_horizontal"] = true;
    if (camera.flip.vertical)
      node[source]["flip_vertical"] = true;
    if (camera.source_size)
      for (int dimension : *camera.source_size)
        node[source]["source_size"].push_back(dimension);
    if (include_policies) {
      if (camera.frozen.has_value())
        node[source]["auto_input_lens_frozen"] = *camera.frozen;
      if (!camera.configuration.empty())
        node[source]["camera_config"] = camera.configuration;
      if (camera.auto_fov.has_value())
        node[source]["camera_automation"]["fov"] = *camera.auto_fov;
      if (camera.auto_lens.has_value())
        node[source]["camera_automation"]["lens"] = *camera.auto_lens;
    }
  }
  return node;
}

std::string SerializeStitchInputCameras(const StitchInputCameras& cameras) {
  YAML::Emitter output;
  output.SetDoublePrecision(17);
  output << YAML::Flow << WriteStitchInputCameras(cameras, false);
  return output.c_str();
}

absl::StatusOr<StitchInputCameras> ParseStitchInputCameras(const std::string& text) {
  try {
    return ReadStitchInputCameras(YAML::Load(text));
  } catch (const YAML::Exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

int StitchInputLensProjection(const StitchInputLens& lens) {
  return lens.projection == "full-frame-fisheye" ? 3 : 0;
}

namespace {
absl::StatusOr<std::string> apply_input_lens(
    const std::string& pto,
    const StitchInputLens& lens,
    const std::array<StitchCameraOptics, 2>* cameras) {
  if (cameras)
    for (const auto& camera : *cameras)
      if (const auto status = ValidateStitchCameraOptics(camera); !status.ok())
        return status;
  if (const auto status = ValidateStitchInputLens(lens); !status.ok())
    return status;
  static const std::regex width_pattern(R"((?:^|\s)w([0-9]{1,5})(?=\s|$))");
  static const std::regex height_pattern(R"((?:^|\s)h([0-9]{1,5})(?=\s|$))");
  std::array<std::array<int, 2>, 2> sizes{};
  size_t count = 0;
  std::istringstream scan(pto);
  for (std::string line; std::getline(scan, line);) {
    if (line.rfind("i ", 0) != 0)
      continue;
    line = line.substr(0, line.find(" n\""));
    std::smatch w, h;
    if (count == sizes.size() || !std::regex_search(line, w, width_pattern) ||
        !std::regex_search(line, h, height_pattern))
      return absl::InvalidArgumentError("Input lens requires two source images with dimensions");
    auto& size = sizes[count++];
    size = {std::stoi(w[1]), std::stoi(h[1])};
    if (size[0] <= 0 || size[1] <= 0 || size[0] > 32768 || size[1] > 32768)
      return absl::InvalidArgumentError("Invalid input lens source-image dimensions");
  }
  if (count != sizes.size())
    return absl::InvalidArgumentError("Input lens calibration requires two source images");
  const bool independent = cameras || sizes[0] != sizes[1];
  if (!independent && lens == StitchInputLens{})
    return pto;
  const bool explicit_variables = independent || lens.optimization != "auto";
  std::string first_fov;
  std::istringstream input(pto);
  std::ostringstream output;
  output.imbue(std::locale::classic());
  size_t images = 0;
  for (std::string line; std::getline(input, line);) {
    // pto_gen enables Hugin's position-only preset. On project load that
    // preset regenerates the variable list and discards our explicit v lines.
    if (explicit_variables && line.rfind("#hugin_optimizerMasterSwitch", 0) == 0)
      continue;
    if (line.rfind("v ", 0) == 0 || line == "v") {
      if (explicit_variables)
        continue;
    }
    if (line.rfind("i ", 0) == 0) {
      if (images >= 2)
        return absl::InvalidArgumentError("Input optics require two source images");
      const auto& current_lens = cameras ? (*cameras)[images].input_lens : lens;
      const auto [w, h] = sizes[images];
      if (independent && !cameras) {
        static const std::regex fov_pattern(R"( v([-+0-9.eE]+)(?=\s|$))");
        std::smatch match;
        if (!images && std::regex_search(line, match, fov_pattern))
          first_fov = match[1];
        if (images && line.find(" v=0") != std::string::npos) {
          if (first_fov.empty())
            return absl::InvalidArgumentError("Linked FOV has no first-camera value");
          line = std::regex_replace(line, std::regex(R"( v=0(?=\s|$))"), " v" + first_fov);
        }
      }
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
      set("f", StitchInputLensProjection(current_lens), false);
      if (cameras)
        set("v", (*cameras)[images].horizontal_fov, false);
      for (size_t i = 0; i < current_lens.distortion.size(); ++i)
        set(std::string(1, 'a' + i).c_str(), current_lens.distortion[i], !independent && images != 0);
      const auto flip = cameras ? (*cameras)[images].flip : media::FrameFlip{};
      set("d", current_lens.center_offset[0] * (flip.horizontal ? -w : w), !independent && images != 0);
      set("e", current_lens.center_offset[1] * (flip.vertical ? -h : h), !independent && images != 0);
      ++images;
    }
    output << line << '\n';
  }
  if (images != 2)
    return absl::InvalidArgumentError("Input lens calibration requires two source images");
  if (explicit_variables) {
    output << "#hugin_optimizerMasterSwitch 0\n";
    output << "v r0 p0 r1 p1 y1";
    if (independent) {
      for (size_t i = 0; i < sizes.size(); ++i) {
        const auto& policy = cameras ? (*cameras)[i].input_lens.optimization : lens.optimization;
        if (policy != "fixed") {
          output << " v" << i << " b" << i << " d" << i << " e" << i;
          if (policy == "auto")
            output << " a" << i << " c" << i;
        }
      }
    } else if (lens.optimization == "refine") {
      output << " v0 b0 d0 e0";
    }
    output << "\nv\n";
  }
  return output.str();
}

} // namespace

absl::StatusOr<std::string> ApplyStitchInputLens(const std::string& pto, const StitchInputLens& lens) {
  return apply_input_lens(pto, lens, nullptr);
}

absl::StatusOr<std::string> ApplyStitchInputCameras(
    const std::string& pto,
    const std::array<StitchCameraOptics, 2>& cameras) {
  return apply_input_lens(pto, cameras[0].input_lens, &cameras);
}

} // namespace hm::stitching
