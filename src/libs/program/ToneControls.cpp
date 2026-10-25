#include "hstream/src/libs/program/ToneControls.h"

#include <cmath>

namespace hm::program {
const ToneControlSpec* FindToneControl(std::string_view id) {
  for (const auto& spec : kToneControls)
    if (id == spec.id)
      return &spec;
  return nullptr;
}

absl::StatusOr<TonePropertyCommand> BuildTonePropertyCommand(
    std::string_view id,
    double presentation_value,
    bool high_bit_depth) {
  const auto* spec = FindToneControl(id);
  if (!spec)
    return absl::InvalidArgumentError("Unknown tone control");
  if (!std::isfinite(presentation_value) || presentation_value < spec->minimum || presentation_value > spec->maximum ||
      std::trunc(presentation_value) != presentation_value)
    return absl::InvalidArgumentError("Tone control requires an integral value within its presentation range");
  const int value = static_cast<int>(presentation_value);
  std::string wire_value = std::to_string(value);
  if (spec->divisor == 100) {
    // Match QString::number(value / 100.0, 'f', 2) without locale or rounding
    // dependencies. All admitted tone values are nonnegative integers.
    const int fraction = value % spec->divisor;
    wire_value = std::to_string(value / spec->divisor) + "." + (fraction < 10 ? "0" : "") + std::to_string(fraction);
  }
  return TonePropertyCommand{high_bit_depth ? "hmstitcher0" : "playcropper0", spec->property, wire_value};
}
} // namespace hm::program
