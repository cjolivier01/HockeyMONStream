#pragma once

#include <array>
#include <optional>
#include <string>
#include <vector>

#include <yaml-cpp/yaml.h>

#include "absl/status/status.h"
#include "absl/status/statusor.h"

namespace hm::stitching {

enum class MappingBackend {
  kNona,
  kOpenCvMagsac,
  kOpenCvAffineRansac,
};

const char* MappingBackendName(MappingBackend backend);
absl::StatusOr<MappingBackend> ParseMappingBackend(const std::string& value);

// How the live CUDA stitcher combines the remapped cameras. This is a render-time choice only; it
// does not affect calibration, so it must stay out of StitchingBackendChoices.
//
// kLaplacian mixes the cameras across every spatial scale in the overlap, which hides exposure
// differences but also softens the detail the detector sees. kAlpha crossfades over a narrow band
// around the seam and leaves the rest of the overlap untouched. kHardSeam does not mix at all.
enum class BlendMode {
  kLaplacian,
  kAlpha,
  kHardSeam,
};

const char* BlendModeName(BlendMode mode);
absl::StatusOr<BlendMode> ParseBlendMode(const std::string& value);

struct ResolvedBlendSettings {
  // Unsupported values are retained so callers can show them and require an explicit choice.
  std::string mode{"laplacian"};
  double feather_fraction{0.05};
};

// Resolve explicit layers in increasing precedence. At one layer native dashed properties win
// over their underscore aliases, private-properties aliases, and canonical stitching keys.
// Null means inherit.
absl::StatusOr<ResolvedBlendSettings> ResolveBlendSettings(const std::vector<YAML::Node>& layers);
absl::StatusOr<ResolvedBlendSettings> ResolveBlendSettings(
    const YAML::Node& baseline,
    const YAML::Node& user = YAML::Node(),
    const YAML::Node& game = YAML::Node());

// Save an explicit choice and retire native aliases that would otherwise override it at this layer.
absl::Status WriteBlendSettings(
    YAML::Node config,
    const std::string& mode,
    const std::optional<double>& feather_fraction = std::nullopt);

enum class StitchProjection {
  kRectilinear,
  kCylindrical,
  kEquirectangular,
  kFullFrameFisheye,
  kStereographic,
  kMercator,
  kTransverseMercator,
  kSinusoidal,
  kLambertCylindricalEqualArea,
  kLambertAzimuthalEqualArea,
  kAlbersEqualAreaConic,
  kMillerCylindrical,
  kPanini,
  kArchitectural,
  kOrthographic,
  kEquisolid,
  kEquirectangularPanini,
  kBiplane,
  kTriplane,
  kGeneralPanini,
  kThoby,
  kHammerAitoff,
};

struct StitchProjectionInfo {
  StitchProjection projection;
  const char* name;
  const char* display_name;
  int hugin_projection;
};

struct StitchProjectionParameterInfo {
  const char* name;
  const char* display_name;
  double minimum;
  double maximum;
  double default_value;
  const char* description;
};

const std::array<StitchProjectionInfo, 22>& SupportedStitchProjections();
const StitchProjectionInfo& StitchProjectionDetails(StitchProjection projection);
const char* StitchProjectionName(StitchProjection projection);
absl::StatusOr<StitchProjection> ParseStitchProjection(const std::string& value);
const std::vector<StitchProjectionParameterInfo>& StitchProjectionParameters(StitchProjection projection);
std::vector<double> DefaultStitchProjectionParameters(StitchProjection projection);
absl::Status ValidateStitchProjectionParameters(StitchProjection projection, const std::vector<double>& parameters);
// Mirrors the horizontal FOV limits reported by Hugin/libpano for the
// selected projection. Parameterized projections such as Biplane, Triplane,
// and General Panini derive their limit from the active parameter values.
absl::StatusOr<double> MaximumStitchProjectionHorizontalFov(
    StitchProjection projection,
    const std::vector<double>& parameters);
absl::Status ValidateStitchProjectionHorizontalFov(
    StitchProjection projection,
    const std::vector<double>& parameters,
    double horizontal_fov);
std::string FormatStitchProjectionParameters(const std::vector<double>& parameters, char separator = ',');
absl::StatusOr<std::vector<double>> ParseStitchProjectionParameters(
    StitchProjection projection,
    const std::string& value);
bool MappingBackendSupportsProjection(MappingBackend backend, StitchProjection projection);
absl::Status ValidateMappingBackendProjection(MappingBackend backend, StitchProjection projection);

} // namespace hm::stitching
