#include "hstream/src/libs/program/RotationControls.h"

#include "hstream/src/libs/common/ConfigPath.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace hm::program {

bool ValidRotationControls(
    const RotationControlValues& controls,
    bool complete,
    const RotationControlValues* range_reference) {
  if (controls.empty() || (complete && controls.size() != kRotationControls.size()))
    return false;
  for (const auto& [id, value] : controls) {
    const auto spec = std::find_if(
        kRotationControls.begin(), kRotationControls.end(), [&](const auto& spec) { return id == spec.id; });
    if (spec == kRotationControls.end())
      return false;
    const auto reference = range_reference ? range_reference->find(id) : controls.end();
    const bool extended = range_reference && reference != range_reference->end();
    const int minimum = extended ? std::min(spec->minimum, reference->second) : spec->minimum;
    const int maximum = extended ? std::max(spec->maximum, reference->second) : spec->maximum;
    if (value < minimum || value > maximum || (id == "Link_Fixed_Edge_Rotation_Left_Right" && value != 0 && value != 1))
      return false;
  }
  return true;
}

absl::StatusOr<RotationControlValues> ReadRotationControls(const YAML::Node& resolved) {
  try {
    const auto required = [&](const char* path) {
      const auto node = hm::get_node(resolved, path);
      if (!node)
        throw std::invalid_argument(std::string("Missing rotation setting ") + path);
      return *node;
    };
    const auto rounded = [](const YAML::Node& value, double scale, double offset) {
      if (!value.IsScalar())
        throw std::invalid_argument("Rotation requires a finite number");
      const double number = value.as<double>() * scale + offset;
      if (!std::isfinite(number) || number < std::numeric_limits<int>::min() ||
          number > std::numeric_limits<int>::max())
        throw std::invalid_argument("Rotation is outside its native bounds");
      return static_cast<int>(std::lround(number));
    };
    const auto rotation = required("stitching.post_stitch_rotate_degrees");
    const auto fixed = required("rink.camera.fixed_edge_rotation_angle");
    RotationControlValues result{{"Stitch_Rotate_Degrees", rotation.IsNull() ? 90 : rounded(rotation, -1, 90)}};
    const bool paired = fixed.IsSequence() && fixed.size() == 2;
    if (!fixed.IsNull() && !fixed.IsScalar() && !paired)
      throw std::invalid_argument("Fixed-edge rotation requires null, an angle, or a left/right pair");
    result["Link_Fixed_Edge_Rotation_Left_Right"] = !paired;
    result["Left_Fixed_Edge_Rotation_Angle_x10"] = fixed.IsNull() ? 0 : rounded(paired ? fixed[0] : fixed, 10, 0);
    result["Right_Fixed_Edge_Rotation_Angle_x10"] = fixed.IsNull() ? 0 : rounded(paired ? fixed[1] : fixed, 10, 0);
    if (!ValidRotationControls(result, true, &result))
      throw std::invalid_argument("Rotation is outside its native bounds");
    return result;
  } catch (const std::exception& error) {
    return absl::InvalidArgumentError(error.what());
  }
}

absl::StatusOr<RotationControlValues> MergeRotationControls(
    const RotationControlValues& current,
    const RotationControlValues& edits) {
  if (!ValidRotationControls(current, true, &current) || !ValidRotationControls(edits, false, &current))
    return absl::InvalidArgumentError("Rotation controls must use native steps within Qt bounds");
  auto result = current;
  for (const auto& [id, value] : edits)
    result[id] = value;
  if (result.at("Link_Fixed_Edge_Rotation_Left_Right")) {
    const auto right = edits.find("Right_Fixed_Edge_Rotation_Angle_x10");
    const auto left = edits.find("Left_Fixed_Edge_Rotation_Angle_x10");
    if (left != edits.end() && right != edits.end() && left->second != right->second)
      return absl::InvalidArgumentError("Linked fixed-edge angles must agree");
    if (right != edits.end() && left == edits.end())
      result["Left_Fixed_Edge_Rotation_Angle_x10"] = right->second;
    result["Right_Fixed_Edge_Rotation_Angle_x10"] = result.at("Left_Fixed_Edge_Rotation_Angle_x10");
  }
  return result;
}

absl::StatusOr<std::vector<RotationPropertyCommand>> FixedEdgeRotationCommands(
    const RotationControlValues& values,
    const std::set<std::string>& changed,
    bool suppressed) {
  if (!ValidRotationControls(values, true, &values))
    return absl::InvalidArgumentError("Missing native rotation readback");
  std::vector<RotationPropertyCommand> result;
  const bool fixed_changed =
      std::any_of(changed.begin(), changed.end(), [](const auto& id) { return id != "Stitch_Rotate_Degrees"; });
  if (!fixed_changed)
    return result;
  const auto add = [&](const char* property, int tenths) {
    std::ostringstream text;
    text.imbue(std::locale::classic());
    text << std::fixed << std::setprecision(1) << (suppressed ? 0 : tenths) / 10.0;
    for (const char* element : {"dsplaytracker0", "playcropper0"})
      result.push_back({element, property, text.str()});
  };
  if (values.at("Link_Fixed_Edge_Rotation_Left_Right")) {
    add("fixed-edge-rotation-angle", values.at("Left_Fixed_Edge_Rotation_Angle_x10"));
  } else {
    add("fixed-edge-rotation-angle-left", values.at("Left_Fixed_Edge_Rotation_Angle_x10"));
    add("fixed-edge-rotation-angle-right", values.at("Right_Fixed_Edge_Rotation_Angle_x10"));
  }
  return result;
}

} // namespace hm::program
