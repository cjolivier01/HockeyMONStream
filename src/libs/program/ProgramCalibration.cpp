#include "hstream/src/libs/program/ProgramCalibration.h"

#include <charconv>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string_view>

#include "hstream/src/libs/common/ConfigPath.h"
#include "hstream/src/libs/stitching/GameConfig.h"

namespace hm::program {
namespace {
// Calibration identity needs exact decimal comparison. The general playback
// parser uses floating point and can differ by a nanosecond across spellings.
uint64_t ReferenceNanoseconds(std::string_view text) {
  const auto invalid = [] { throw std::invalid_argument("Expected nonnegative [HH:]MM:SS or decimal seconds"); };
  constexpr uint64_t billion = 1000000000;
  constexpr auto maximum = std::numeric_limits<uint64_t>::max();
  uint64_t seconds = 0, fraction = 0;
  unsigned components = 0;
  for (;;) {
    if (++components > 3 || text.empty())
      invalid();
    const auto colon = text.find(':');
    auto component = text.substr(0, colon);
    if (colon == std::string_view::npos) {
      const auto dot = component.find('.');
      if (dot != std::string_view::npos) {
        auto digits = component.substr(dot + 1);
        if (digits.empty())
          invalid();
        uint64_t place = billion;
        for (const char digit : digits) {
          if (digit < '0' || digit > '9')
            invalid();
          place /= 10;
          if (!place && digit != '0')
            invalid(); // Do not silently admit a different subnanosecond reference.
          fraction += uint64_t(digit - '0') * place;
        }
        component = component.substr(0, dot);
      }
    }
    uint64_t value = 0;
    const auto parsed = std::from_chars(component.data(), component.data() + component.size(), value);
    if (parsed.ec != std::errc{} || parsed.ptr != component.data() + component.size() || value > maximum / billion ||
        seconds > (maximum / billion - value) / 60)
      invalid();
    seconds = seconds * 60 + value;
    if (colon == std::string_view::npos)
      break;
    text.remove_prefix(colon + 1);
  }
  if (seconds > (maximum - fraction) / billion)
    invalid();
  return seconds * billion + fraction;
}
} // namespace
absl::Status ValidateProgramCalibrationSelection(const YAML::Node& saved, const ProgramCalibrationRequest& request) {
  try {
    if (request.reference_time) {
      const auto reference = get_node(saved, "stitching.stitch_frame_time");
      if (!reference || !reference->IsScalar() ||
          ReferenceNanoseconds(reference->as<std::string>()) != ReferenceNanoseconds(*request.reference_time))
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
