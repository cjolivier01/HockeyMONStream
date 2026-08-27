#include "hstream/src/libs/stitching/StitchingAlgorithms.h"

#include <algorithm>
#include <cctype>
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
