#include "hstream/src/libs/program/RotationControls.h"

#include <cstdlib>
#include <iostream>

namespace {
void Check(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
    std::abort();
  }
}
} // namespace

int main() {
  using namespace hm::program;
  auto config = YAML::LoadFile("configs/baseline.yaml");
  const auto initial = ReadRotationControls(config);
  Check(initial.ok() && initial->size() == 4, "Canonical rotation settings were rejected");
  config["stitching"].remove("post_stitch_rotate_degrees");
  Check(!ReadRotationControls(config).ok(), "Missing rotation used an independent default");
  config["stitching"]["post_stitch_rotate_degrees"] = 17;
  config["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Load("[21.5, 32.7]");
  const auto separate = ReadRotationControls(config);
  Check(
      separate.ok() && separate->at("Stitch_Rotate_Degrees") == 73 &&
          separate->at("Left_Fixed_Edge_Rotation_Angle_x10") == 215 &&
          separate->at("Right_Fixed_Edge_Rotation_Angle_x10") == 327 &&
          separate->at("Link_Fixed_Edge_Rotation_Left_Right") == 0,
      "Rotation units or independent left/right selection diverged");
  const auto commands = FixedEdgeRotationCommands(*separate, {"Right_Fixed_Edge_Rotation_Angle_x10"}, false);
  Check(
      commands.ok() && commands->size() == 4 && (*commands)[0].element == "dsplaytracker0" &&
          (*commands)[1].element == "playcropper0" && (*commands)[0].value == "21.5" &&
          (*commands)[2].property == "fixed-edge-rotation-angle-right" && (*commands)[2].value == "32.7",
      "Independent rotation did not publish both native stages in tenths");
  const auto linked = MergeRotationControls(*separate, {{"Link_Fixed_Edge_Rotation_Left_Right", 1}});
  Check(
      linked.ok() && linked->at("Right_Fixed_Edge_Rotation_Angle_x10") == 215,
      "Enabling Link did not retain the left angle");
  const auto mirrored = MergeRotationControls(*linked, {{"Right_Fixed_Edge_Rotation_Angle_x10", 127}});
  Check(
      mirrored.ok() && mirrored->at("Left_Fixed_Edge_Rotation_Angle_x10") == 127,
      "A linked right-angle edit did not mirror left");
  Check(
      !MergeRotationControls(
           *linked, {{"Left_Fixed_Edge_Rotation_Angle_x10", 120}, {"Right_Fixed_Edge_Rotation_Angle_x10", 121}})
           .ok(),
      "Contradictory linked angles were admitted");
  const auto neutral = FixedEdgeRotationCommands(*mirrored, {"Link_Fixed_Edge_Rotation_Left_Right"}, true);
  Check(
      neutral.ok() && neutral->size() == 4 && neutral->front().value == "0.0",
      "Leveled nona suppression did not publish a neutral angle");
  Check(
      FixedEdgeRotationCommands(*separate, {"Stitch_Rotate_Degrees"}, false)->empty(),
      "Stitch rotation changed fixed-edge properties");
  config["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Load("[-12.3, 9.4]");
  const auto legacy = ReadRotationControls(config);
  Check(
      legacy.ok() && legacy->at("Left_Fixed_Edge_Rotation_Angle_x10") == -123 &&
          MergeRotationControls(*legacy, {{"Left_Fixed_Edge_Rotation_Angle_x10", -100}}).ok() &&
          !MergeRotationControls(*legacy, {{"Left_Fixed_Edge_Rotation_Angle_x10", -124}}).ok(),
      "Saved negative angles did not extend only their existing Qt slider range");
  const auto legacy_changed = MergeRotationControls(*legacy, {{"Left_Fixed_Edge_Rotation_Angle_x10", -100}});
  Check(
      legacy_changed.ok() &&
          MergeRotationControls(*legacy_changed, {{"Left_Fixed_Edge_Rotation_Angle_x10", -123}}, &*legacy).ok(),
      "An edit must not shrink the session's original saved angle range");
  config["rink"]["camera"]["fixed_edge_rotation_angle"] = YAML::Load("[1, 2, 3]");
  Check(!ReadRotationControls(config).ok(), "Malformed angle tuple was accepted");
  return 0;
}
