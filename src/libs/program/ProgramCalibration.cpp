#include "hstream/src/libs/program/ProgramCalibration.h"

#include <exception>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/common/utils.h"
#include "hstream/src/libs/stitching/GameConfig.h"

namespace hm::program {
absl::Status ValidateProgramCalibrationSelection(const YAML::Node& saved, const ProgramCalibrationRequest& request) {
  try {
    if (request.reference_time) {
      const auto reference = get_node(saved, "stitching.stitch_frame_time");
      if (!reference || !reference->IsScalar() ||
          hhmmss_to_nanoseconds(reference->as<std::string>()) != hhmmss_to_nanoseconds(*request.reference_time))
        return absl::FailedPreconditionError(
            "AMD playback requires --stitch-frame-time to match the saved calibration reference");
    }
    if (request.invalidation_id) {
      const auto owner = get_node(saved, "hstream_ui.stitching_calibration.invalidation_id");
      const auto state = get_node(saved, "hstream_ui.stitching_calibration.status");
      const auto invalidated = get_node(saved, "hstream_ui.stitching_calibration.artifacts_invalidated");
      if (request.invalidation_id->empty() || !owner || !owner->IsScalar() ||
          owner->as<std::string>() != *request.invalidation_id || !state || !state->IsScalar() ||
          state->as<std::string>() != "complete" || (invalidated && invalidated->as<bool>()))
        return absl::FailedPreconditionError(
            "AMD playback requires the requested completed calibration revision; automatic calibration is unavailable");
    }
    return absl::OkStatus();
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(std::string("Invalid saved Program calibration selection: ") + error.what());
  }
}

absl::Status ValidateLockedProgramCalibration(
    const std::filesystem::path& game,
    const ProgramCalibrationRequest& request,
    const stitching::HuginProject::ArtifactLock&) {
  if (!request.reference_time && !request.invalidation_id)
    return absl::OkStatus();
  auto saved = stitching::load_game_config_file(game / "config.yaml");
  if (!saved.ok())
    return saved.status();
  if (!*saved)
    return absl::FailedPreconditionError("Saved Program calibration disappeared before artifact loading");
  return ValidateProgramCalibrationSelection(**saved, request);
}
} // namespace hm::program
