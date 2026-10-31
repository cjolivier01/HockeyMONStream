#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "hstream/src/libs/stitching/GameConfig.h"

namespace hm::stitching {

struct VideoLensMetadata {
  StitchCameraSelection camera;
  std::string model;
  std::string stabilization;
  std::string mode;
  size_t width{0};
  size_t height{0};
  // Camera/settings remain available for mismatch warnings even when its warp
  // cannot be represented by the shared Hugin model.
  std::string unavailable_reason;
  // Coverage in the recorded pixels, independent of Hugin representability.
  std::optional<double> horizontal_fov;
  std::optional<double> vertical_fov;
  std::string fov_unavailable_reason;
  std::map<std::string, std::string> recording_mode;
};

// Bounded MP4 header/trailer and first DJI metadata sample reads only.
// Never decodes video or scans mdat.
// Results are cached by file identity, size and modification/change timestamps.
absl::StatusOr<VideoLensMetadata> ReadVideoLensMetadata(const std::filesystem::path& video);
// Exposed for bounded malformed-input and model-conversion tests.
absl::StatusOr<VideoLensMetadata> ParseVideoLensMetadata(const std::vector<unsigned char>& gpmf);

absl::StatusOr<VideoLensMetadata> ParseInsta360LensMetadata(const std::vector<unsigned char>& data);
absl::StatusOr<VideoLensMetadata> ParseDjiLensMetadata(
    const std::vector<unsigned char>& data,
    size_t width,
    size_t height);

struct InputCameraMetadata {
  std::string role;
  std::string message;
  std::string fov_message;
  std::string lens_message;
  std::string recording_message;
  bool mismatch{false};
  bool recording_metadata_found{false};
  bool horizontal_fov_from_mp4{false};
  bool vertical_fov_from_mp4{false};
  bool lens_from_mp4{false};
};

struct AutoInputLensResult {
  StitchCameraSelection camera;
  std::string message;
  bool detected{false};
  bool mismatch{false};
  std::string fov_message;
  std::string lens_message;
  // Presentation provenance, never persisted as calibration inputs. Recognition
  // of a recording is independent of importing either component of its optics.
  std::string recording_message;
  bool recording_metadata_found{false};
  bool horizontal_fov_from_mp4{false};
  bool vertical_fov_from_mp4{false};
  bool lens_from_mp4{false};
  // Read-only source-camera diagnostics, including independent prescriptions.
  std::string camera_details;
  // Source-bound controls use these individual statuses instead of treating
  // differences between cameras as a metadata error.
  std::map<std::string, InputCameraMetadata> source_metadata;
  bool panorama{false};
};

// Inspects every selected chapter. Only agrees to a shared prescription when
// every input has compatible metadata. Missing/unsupported/different metadata
// leaves automatic fitting in use, with a visible reason for the operator.
AutoInputLensResult ResolveAutoInputLens(
    const std::vector<std::filesystem::path>& videos,
    const StitchCameraSelection& fallback,
    const std::vector<StitchCameraConfiguration>& profiles = {});

AutoInputLensResult ResolveAutoInputLensForCameras(
    const std::vector<std::filesystem::path>& left,
    const std::vector<std::filesystem::path>& right,
    const StitchCameraSelection& fallback,
    const std::vector<StitchCameraConfiguration>& profiles = {},
    bool roles_known = true,
    const std::filesystem::path& game = {},
    bool frozen = false);

} // namespace hm::stitching
