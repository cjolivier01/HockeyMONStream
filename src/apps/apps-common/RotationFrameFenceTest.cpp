#include "hstream/src/apps/apps-common/RotationFrameFence.h"

#include <iostream>
#include <limits>

namespace {
bool expect(bool value, const char* message) {
  if (!value)
    std::cerr << message << '\n';
  return value;
}
} // namespace

int main() {
  using namespace hm::preview;
  bool ok = true;
  RotationFrameFence selected{"epoch-b", {{12.7F, 32.5F}}};
  RotationFrameObservation frame{true, "epoch-a", true, 12.7F, 32.5F};
  ok &= expect(!selected.Matches("program", frame), "Old native authorization must not acquire a new operation");
  frame.authorization_id = "epoch-b";
  ok &= expect(selected.Matches("program", frame), "Exact epoch and crop are admitted");
  frame.left = 12.75F;
  ok &= expect(!selected.Matches("program", frame), "Rounded UI equality cannot admit an older fractional crop");
  ok &= expect(selected.Matches("stitched", frame), "Crop angles do not affect Stitched pixels");
  frame.epoch_present = false;
  ok &= expect(!selected.Matches("stitched", frame), "Missing native epoch fails closed");
  ok &= expect(selected.Matches("source0", frame), "Source pixels are independent of rotation");
  frame.epoch_present = true;
  frame.left = std::numeric_limits<float>::quiet_NaN();
  ok &= expect(!selected.Matches("program", frame), "Invalid crop metadata fails closed");
  frame.crop_present = false;
  ok &= expect(!selected.Matches("program", frame), "Explicit crop edits require producer settings");
  selected.angles.reset();
  ok &= expect(selected.Matches("program", frame), "Stitch-only edit preserves unrounded legacy automatic crop");
  ok &= expect(!selected.Matches("other", frame), "Unknown routes are rejected");
  return ok ? 0 : 1;
}
