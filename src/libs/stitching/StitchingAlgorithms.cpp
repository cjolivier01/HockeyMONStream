#include "hstream/src/libs/stitching/StitchingAlgorithms.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace hm::stitching {
namespace {

std::string normalize_choice(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return character == '_' ? '-' : static_cast<char>(std::tolower(character));
  });
  return value;
}

constexpr std::array<StitchProjectionInfo, 22> kProjections = {{
    {StitchProjection::kRectilinear, "rectilinear", "Rectilinear", 0},
    {StitchProjection::kCylindrical, "cylindrical", "Cylindrical", 1},
    {StitchProjection::kEquirectangular, "equirectangular", "Equirectangular", 2},
    {StitchProjection::kFullFrameFisheye, "full-frame-fisheye", "Full-frame fisheye", 3},
    {StitchProjection::kStereographic, "stereographic", "Stereographic", 4},
    {StitchProjection::kMercator, "mercator", "Mercator", 5},
    {StitchProjection::kTransverseMercator, "transverse-mercator", "Transverse Mercator", 6},
    {StitchProjection::kSinusoidal, "sinusoidal", "Sinusoidal", 7},
    {StitchProjection::kLambertCylindricalEqualArea,
     "lambert-cylindrical-equal-area",
     "Lambert cylindrical equal-area",
     8},
    {StitchProjection::kLambertAzimuthalEqualArea, "lambert-azimuthal-equal-area", "Lambert azimuthal equal-area", 9},
    {StitchProjection::kAlbersEqualAreaConic, "albers-equal-area-conic", "Albers equal-area conic", 10},
    {StitchProjection::kMillerCylindrical, "miller-cylindrical", "Miller cylindrical", 11},
    {StitchProjection::kPanini, "panini", "Panini", 12},
    {StitchProjection::kArchitectural, "architectural", "Architectural", 13},
    {StitchProjection::kOrthographic, "orthographic", "Orthographic", 14},
    {StitchProjection::kEquisolid, "equisolid", "Equisolid", 15},
    {StitchProjection::kEquirectangularPanini,
     "equirectangular-panini",
     "Equirectangular Panini",
     16},
    {StitchProjection::kBiplane, "biplane", "Biplane", 17},
    {StitchProjection::kTriplane, "triplane", "Triplane", 18},
    {StitchProjection::kGeneralPanini, "general-panini", "General Panini", 19},
    {StitchProjection::kThoby, "thoby", "Thoby", 20},
    {StitchProjection::kHammerAitoff, "hammer-aitoff", "Hammer-Aitoff", 21},
}};

const std::vector<StitchProjectionParameterInfo> kNoProjectionParameters;
const std::vector<StitchProjectionParameterInfo> kAlbersParameters = {
    {"phi1",
     "First standard parallel (phi1)",
     -90.0,
     90.0,
     0.0,
     "First standard parallel, in degrees, for the Albers equal-area conic projection. Scale is exact along this "
     "latitude. Hugin range: -90 to 90; default: 0."},
    {"phi2",
     "Second standard parallel (phi2)",
     -90.0,
     90.0,
     60.0,
     "Second standard parallel, in degrees, for the Albers equal-area conic projection. Together with phi1 it "
     "sets the cone and the latitudes with exact scale. Hugin range: -90 to 90; default: 60."},
};
const std::vector<StitchProjectionParameterInfo> kBiplaneParameters = {
    {"alpha",
     "Plane angle (alpha)",
     1.0,
     179.0,
     45.0,
     "Angle, in degrees, controlling the two planes of the Biplane projection. Smaller or larger values move the "
     "join and change side compression. Hugin range: 1 to 179; default: 45."},
    {"corners",
     "Rounded corners",
     0.0,
     1.0,
     0.0,
     "Biplane rounded-corner switch. 0 keeps a sharp join between the two rectilinear planes; 1 rounds the central "
     "join with a cylindrical section. Hugin range: 0 or 1; default: 0."},
};
const std::vector<StitchProjectionParameterInfo> kTriplaneParameters = {
    {"alpha",
     "Plane angle (alpha)",
     1.0,
     120.0,
     60.0,
     "Angle, in degrees, controlling the three planes of the Triplane projection. It changes where the side "
     "planes meet the center plane and their compression. Hugin range: 1 to 120; default: 60."},
};
const std::vector<StitchProjectionParameterInfo> kGeneralPaniniParameters = {
    {"Cmpr",
     "Compression (Cmpr)",
     0.0,
     150.0,
     100.0,
     "Horizontal compression for General Panini. 0 is rectilinear, 100 is standard Panini, and 150 is cylindrical "
     "orthographic. Maximum horizontal field of view varies from about 160 degrees at 0, to 320 degrees at 100, "
     "to 180 degrees at 150. Hugin range: 0 to 150; default: 100."},
    {"Tops",
     "Top squeeze (Tops)",
     -100.0,
     100.0,
     0.0,
     "Vertical squeeze for the top half of a General Panini image; it controls straightening of horizontal lines. "
     "0 applies no squeeze. Positive values use hard squeeze, which can exactly straighten lines but is limited to "
     "roughly 160 degrees. Negative values use soft squeeze, which supports wider fields of view but cannot remove "
     "all curvature. Hugin range: -100 to 100; default: 0."},
    {"Bots",
     "Bottom squeeze (Bots)",
     -100.0,
     100.0,
     0.0,
     "Vertical squeeze for the bottom half of a General Panini image; it controls straightening of horizontal "
     "lines. 0 applies no squeeze. Positive values use hard squeeze, which can exactly straighten lines but is "
     "limited to roughly 160 degrees. Negative values use soft squeeze, which supports wider fields of view but "
     "cannot remove all curvature. Hugin range: -100 to 100; default: 0."},
};

} // namespace

const char* MappingBackendName(MappingBackend backend) {
  switch (backend) {
    case MappingBackend::kNona:
      return "nona";
    case MappingBackend::kOpenCvMagsac:
      return "opencv-magsac";
    case MappingBackend::kOpenCvAffineRansac:
      return "opencv-affine-ransac";
  }
  return "nona";
}

absl::StatusOr<MappingBackend> ParseMappingBackend(const std::string& value) {
  const std::string normalized = normalize_choice(value.empty() ? "opencv-magsac" : value);
  if (normalized == "nona")
    return MappingBackend::kNona;
  if (normalized == "opencv-magsac" || normalized == "magsac" || normalized == "magsac++")
    return MappingBackend::kOpenCvMagsac;
  if (normalized == "opencv-affine-ransac" || normalized == "affine-ransac" || normalized == "ransac")
    return MappingBackend::kOpenCvAffineRansac;
  return absl::InvalidArgumentError(
      "Unsupported stitching mapping backend \"" + value +
      "\"; choose nona, opencv-magsac/MAGSAC++, or opencv-affine-ransac/RANSAC");
}

const std::array<StitchProjectionInfo, 22>& SupportedStitchProjections() {
  return kProjections;
}

const StitchProjectionInfo& StitchProjectionDetails(StitchProjection projection) {
  for (const auto& info : kProjections) {
    if (info.projection == projection)
      return info;
  }
  throw std::invalid_argument("Unknown stitching projection");
}

const char* StitchProjectionName(StitchProjection projection) {
  return StitchProjectionDetails(projection).name;
}

absl::StatusOr<StitchProjection> ParseStitchProjection(const std::string& value) {
  std::string normalized = normalize_choice(value.empty() ? "general-panini" : value);
  if (normalized == "rectilinear" || normalized == "planar")
    return StitchProjection::kRectilinear;
  if (normalized == "general-panini" || normalized == "panini-general" || normalized == "panini-generalized")
    return StitchProjection::kGeneralPanini;
  if (normalized == "hammer" || normalized == "hammer-aitoff")
    return StitchProjection::kHammerAitoff;
  if (normalized == "fullframe-fisheye")
    normalized = "full-frame-fisheye";
  for (const auto& info : kProjections) {
    if (normalized == info.name)
      return info.projection;
  }
  return absl::InvalidArgumentError("Unsupported stitching projection \"" + value + "\"");
}

const std::vector<StitchProjectionParameterInfo>& StitchProjectionParameters(StitchProjection projection) {
  switch (projection) {
    case StitchProjection::kAlbersEqualAreaConic:
      return kAlbersParameters;
    case StitchProjection::kBiplane:
      return kBiplaneParameters;
    case StitchProjection::kTriplane:
      return kTriplaneParameters;
    case StitchProjection::kGeneralPanini:
      return kGeneralPaniniParameters;
    default:
      return kNoProjectionParameters;
  }
}

std::vector<double> DefaultStitchProjectionParameters(StitchProjection projection) {
  std::vector<double> defaults;
  for (const auto& parameter : StitchProjectionParameters(projection))
    defaults.push_back(parameter.default_value);
  return defaults;
}

absl::Status ValidateStitchProjectionParameters(StitchProjection projection, const std::vector<double>& parameters) {
  const auto& definitions = StitchProjectionParameters(projection);
  if (parameters.size() != definitions.size()) {
    return absl::InvalidArgumentError(
        "stitching projection \"" + std::string(StitchProjectionName(projection)) + "\" requires " +
        std::to_string(definitions.size()) + " projection parameter" + (definitions.size() == 1 ? "" : "s") +
        "; received " + std::to_string(parameters.size()));
  }
  for (size_t index = 0; index < definitions.size(); ++index) {
    const auto& definition = definitions[index];
    if (!std::isfinite(parameters[index]) || parameters[index] < definition.minimum ||
        parameters[index] > definition.maximum) {
      return absl::InvalidArgumentError(
          "stitching projection parameter " + std::string(definition.name) + " for \"" +
          StitchProjectionName(projection) + "\" must be finite and between " + std::to_string(definition.minimum) +
          " and " + std::to_string(definition.maximum));
    }
    if (std::string_view(definition.name) == "corners" && parameters[index] != 0.0 && parameters[index] != 1.0) {
      return absl::InvalidArgumentError(
          "stitching projection parameter corners for \"" + std::string(StitchProjectionName(projection)) +
          "\" must be exactly 0 or 1");
    }
    // The UI exposes hundredths, and every supported value at that precision
    // round-trips through Hugin's roughly six-significant-digit PTO format.
    // Reject finer YAML/CLI values before a generation claim can record a
    // tuple that pano_modify will silently round to different semantics.
    constexpr double kHundredthsScale = 100.0;
    constexpr double kHundredthsTolerance = 1e-7;
    const double hundredths = parameters[index] * kHundredthsScale;
    if (std::abs(hundredths - std::round(hundredths)) > kHundredthsTolerance) {
      return absl::InvalidArgumentError(
          "stitching projection parameter " + std::string(definition.name) + " for \"" +
          StitchProjectionName(projection) + "\" must use increments of 0.01");
    }
  }
  return absl::OkStatus();
}

absl::StatusOr<double> MaximumStitchProjectionHorizontalFov(
    StitchProjection projection,
    const std::vector<double>& parameters) {
  const absl::Status parameter_status = ValidateStitchProjectionParameters(projection, parameters);
  if (!parameter_status.ok())
    return parameter_status;

  switch (projection) {
    case StitchProjection::kRectilinear:
    case StitchProjection::kTransverseMercator:
      return 179.0;
    case StitchProjection::kStereographic:
    case StitchProjection::kPanini:
    case StitchProjection::kEquirectangularPanini:
      return 359.0;
    case StitchProjection::kOrthographic:
      return 180.0;
    case StitchProjection::kBiplane:
      // libpano queryFOVLimits(): alpha + the 179-degree center plane.
      return std::min(360.0, parameters[0] + 179.0);
    case StitchProjection::kTriplane:
      // libpano queryFOVLimits(): two side-plane angles plus the
      // 179-degree center plane.
      return std::min(360.0, 2.0 * parameters[0] + 179.0);
    case StitchProjection::kGeneralPanini: {
      // This is the dynamic libpano General Panini limit. Cmpr is converted
      // to the projection's working compression and constrained by its
      // hard-coded 80-degree maximum projection angle. Tops and Bots affect
      // vertical squeeze but not libpano's horizontal FOV limit.
      constexpr double kPi = 3.141592653589793238462643383279502884;
      constexpr double kMaximumProjectionAngle = 80.0 * kPi / 180.0;
      const double compression_scale = (150.0 - parameters[0]) / 50.0;
      const double compression = 1.5 / (compression_scale + 0.0001) - 1.5 / 3.0001;
      const double theoretical = std::acos(compression > 1.0 ? -1.0 / compression : -compression);
      const double projection_argument = compression * std::sin(kMaximumProjectionAngle);
      double half_fov = theoretical;
      if (projection_argument <= 1.0) {
        half_fov = std::min(
            half_fov,
            std::asin(std::max(-1.0, projection_argument)) + kMaximumProjectionAngle);
      }
      return 2.0 * half_fov * 180.0 / kPi;
    }
    case StitchProjection::kCylindrical:
    case StitchProjection::kEquirectangular:
    case StitchProjection::kFullFrameFisheye:
    case StitchProjection::kMercator:
    case StitchProjection::kSinusoidal:
    case StitchProjection::kLambertCylindricalEqualArea:
    case StitchProjection::kLambertAzimuthalEqualArea:
    case StitchProjection::kAlbersEqualAreaConic:
    case StitchProjection::kMillerCylindrical:
    case StitchProjection::kArchitectural:
    case StitchProjection::kEquisolid:
    case StitchProjection::kThoby:
    case StitchProjection::kHammerAitoff:
      return 360.0;
  }
  return absl::InvalidArgumentError("Unknown stitching projection");
}

absl::Status ValidateStitchProjectionHorizontalFov(
    StitchProjection projection,
    const std::vector<double>& parameters,
    double horizontal_fov) {
  double maximum_fov = 0.0;
  auto maximum = MaximumStitchProjectionHorizontalFov(projection, parameters);
  if (!maximum.ok())
    return maximum.status();
  maximum_fov = *maximum;
  if (!std::isfinite(horizontal_fov) || horizontal_fov <= 0.0 || horizontal_fov > maximum_fov + 1e-9) {
    std::ostringstream maximum_text;
    maximum_text.imbue(std::locale::classic());
    maximum_text << std::setprecision(12) << maximum_fov;
    return absl::InvalidArgumentError(
        "stitching projection \"" + std::string(StitchProjectionName(projection)) +
        "\" horizontal FOV must be finite, greater than 0, and at most " + maximum_text.str() + " degrees");
  }
  return absl::OkStatus();
}

std::string FormatStitchProjectionParameters(const std::vector<double>& parameters, char separator) {
  std::ostringstream output;
  output.imbue(std::locale::classic());
  output << std::setprecision(std::numeric_limits<double>::max_digits10);
  for (size_t index = 0; index < parameters.size(); ++index) {
    if (index != 0)
      output << separator;
    output << parameters[index];
  }
  return output.str();
}

absl::StatusOr<std::vector<double>> ParseStitchProjectionParameters(
    StitchProjection projection,
    const std::string& value) {
  std::string normalized = value;
  std::replace(normalized.begin(), normalized.end(), ',', ' ');
  std::istringstream input(normalized);
  input.imbue(std::locale::classic());
  std::vector<double> parameters;
  for (double parameter = 0.0; input >> parameter;)
    parameters.push_back(parameter);
  if (!input.eof())
    return absl::InvalidArgumentError("stitching projection parameters must be numeric values");
  const absl::Status validation = ValidateStitchProjectionParameters(projection, parameters);
  if (!validation.ok())
    return validation;
  return parameters;
}

bool MappingBackendSupportsProjection(MappingBackend backend, StitchProjection projection) {
  return backend == MappingBackend::kNona || projection == StitchProjection::kRectilinear;
}

absl::Status ValidateMappingBackendProjection(MappingBackend backend, StitchProjection projection) {
  if (MappingBackendSupportsProjection(backend, projection))
    return absl::OkStatus();
  return absl::InvalidArgumentError(
      "stitching projection \"" + std::string(StitchProjectionName(projection)) + "\" requires mapping backend nona; " +
      std::string(MappingBackendName(backend)) + " supports only rectilinear output");
}

} // namespace hm::stitching
