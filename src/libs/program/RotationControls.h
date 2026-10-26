#pragma once

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "absl/status/statusor.h"
#include "yaml-cpp/yaml.h"

namespace hm::program {

struct RotationControlSpec {
  const char* id;
  int minimum;
  int maximum;
};
inline constexpr std::array<RotationControlSpec, 4> kRotationControls{{
    {"Stitch_Rotate_Degrees", 0, 180},
    {"Link_Fixed_Edge_Rotation_Left_Right", 0, 1},
    {"Left_Fixed_Edge_Rotation_Angle_x10", 0, 900},
    {"Right_Fixed_Edge_Rotation_Angle_x10", 0, 900},
}};
using RotationControlValues = std::map<std::string, int>;

struct RotationPropertyCommand {
  std::string element;
  std::string property;
  std::string value;
};

// Read the resolved canonical values. Null is the native automatic/neutral
// choice; it is not replaced with an independently maintained default set.
absl::StatusOr<RotationControlValues> ReadRotationControls(const YAML::Node& resolved);
bool ValidRotationControls(
    const RotationControlValues& controls,
    bool complete,
    const RotationControlValues* range_reference = nullptr);
// Linked edits are normalized before publication, exactly as Qt mirrors its
// sliders. Fixed-edge units remain tenths through the shared wire conversion.
absl::StatusOr<RotationControlValues> MergeRotationControls(
    const RotationControlValues& current,
    const RotationControlValues& edits,
    const RotationControlValues* range_reference = nullptr);
absl::StatusOr<std::vector<RotationPropertyCommand>> FixedEdgeRotationCommands(
    const RotationControlValues& values,
    const std::set<std::string>& changed,
    bool suppressed);

} // namespace hm::program
