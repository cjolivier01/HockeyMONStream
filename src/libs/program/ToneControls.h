#pragma once

#include <array>
#include <string>
#include <string_view>

#include "absl/status/statusor.h"

namespace hm::program {
// Presentation units and native property mapping, shared by control surfaces.
// Defaults are deliberately absent: resolve them through baseline/config layers.
struct ToneControlSpec {
  const char* id;
  const char* label;
  int minimum;
  int maximum;
  const char* property;
  int divisor;
};

// Keep legacy batch order: shadows, black point, exposure.
inline constexpr std::array<ToneControlSpec, 3> kToneControls{{
    {"Bring_Up_Shadows", "Bring up shadows (%)", 0, 100, "shadow-lift", 1},
    {"Lift_Shadow_Black_Point", "Lift black point too (stronger)", 0, 1, "shadow-lift-black-point", 1},
    {"Exposure_x100", "Exposure x100", 0, 130, "exposure", 100},
}};

const ToneControlSpec* FindToneControl(std::string_view id);

struct TonePropertyCommand {
  std::string element;
  std::string property;
  std::string value;
};

// Accept only an integral presentation value of a known control. The caller
// supplies the active run's resolved precision, never its next-run selection.
// This builds the existing native/HockeyMON wire values; it does not establish
// backend capability, authorize a live mutation, apply it or persist settings.
// Runtime owners still enforce eligibility, generations and acknowledgments.
absl::StatusOr<TonePropertyCommand> BuildTonePropertyCommand(
    std::string_view id,
    double presentation_value,
    bool high_bit_depth);
} // namespace hm::program
