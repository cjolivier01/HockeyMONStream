#pragma once

#include <array>
#include <string>

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

const std::array<StitchProjectionInfo, 22>& SupportedStitchProjections();
const StitchProjectionInfo& StitchProjectionDetails(StitchProjection projection);
const char* StitchProjectionName(StitchProjection projection);
absl::StatusOr<StitchProjection> ParseStitchProjection(const std::string& value);
bool MappingBackendSupportsProjection(MappingBackend backend, StitchProjection projection);
absl::Status ValidateMappingBackendProjection(MappingBackend backend, StitchProjection projection);

} // namespace hm::stitching
