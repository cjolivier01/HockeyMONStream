#pragma once

#include <array>
#include <string>
#include <vector>

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
