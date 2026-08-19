#include "hstream/src/gst-plugins/gst-videoprep/playcropper/ShadowToneCurve.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

} // namespace

int main() {
  using hm::playcropper::evaluate_shadow_lift_curve;
  constexpr float kTolerance = 2e-5f;

  if (!expect(evaluate_shadow_lift_curve(0.0f, 100.0f) == 0.0f, "Shadow lift must preserve exact black") ||
      !expect(evaluate_shadow_lift_curve(0.6f, 100.0f) == 0.6f, "Shadow lift must preserve the shadow boundary") ||
      !expect(evaluate_shadow_lift_curve(1.0f, 100.0f) == 1.0f, "Shadow lift must preserve exact white")) {
    return 1;
  }

  for (int sample = 0; sample <= 10000; ++sample) {
    const float value = static_cast<float>(sample) / 10000.0f;
    if (!expect(
            std::abs(evaluate_shadow_lift_curve(value, 0.0f) - value) <= kTolerance,
            "A zero shadow lift must be a pixel-exact identity")) {
      return 1;
    }
  }

  for (float amount : std::array<float, 4>{25.0f, 50.0f, 75.0f, 100.0f}) {
    float previous = evaluate_shadow_lift_curve(0.0f, amount);
    for (int sample = 1; sample <= 65535; ++sample) {
      const float value = static_cast<float>(sample) / 65535.0f;
      const float lifted = evaluate_shadow_lift_curve(value, amount);
      if (!expect(std::isfinite(lifted), "Shadow lift must remain finite across the full signal range") ||
          !expect(lifted + kTolerance >= previous, "Shadow lift must not introduce a tone reversal") ||
          !expect(lifted + kTolerance >= value, "A positive shadow lift must not darken the input") ||
          !expect(lifted <= 1.0f + kTolerance, "Shadow lift must remain inside the video signal range") ||
          !expect(
              value < hm::playcropper::kShadowLiftVideoStart || std::abs(lifted - value) <= kTolerance,
              "Shadow lift must leave midtones and highlights unchanged")) {
        return 1;
      }
      previous = lifted;
    }
  }

  const std::array<float, 6> ocio_video_inputs = {0.05f, 0.1f, 0.2f, 0.3f, 0.4f, 0.5f};
  const std::array<float, 6> ocio_video_expected = {
      0.115831240f, 0.179128785f, 0.270156212f, 0.343769410f, 0.421291219f, 0.505902849f};
  for (size_t i = 0; i < ocio_video_inputs.size(); ++i) {
    if (!expect(
            std::abs(evaluate_shadow_lift_curve(ocio_video_inputs[i], 100.0f) - ocio_video_expected[i]) <= kTolerance,
            "Shadow lift must match the OpenColorIO VIDEO-style master Shadows reference")) {
      return 1;
    }
  }

  const float dark_sample = 0.1f;
  if (!expect(
          evaluate_shadow_lift_curve(dark_sample, 100.0f) > evaluate_shadow_lift_curve(dark_sample, 50.0f) &&
              evaluate_shadow_lift_curve(dark_sample, 50.0f) > dark_sample,
          "Increasing the slider must progressively reveal more shadow detail") ||
      !expect(
          evaluate_shadow_lift_curve(dark_sample, -10.0f) == dark_sample,
          "Out-of-range negative input must clamp to identity") ||
      !expect(
          std::abs(evaluate_shadow_lift_curve(dark_sample, 150.0f) - evaluate_shadow_lift_curve(dark_sample, 100.0f)) <=
              kTolerance,
          "Out-of-range positive input must clamp to the validated maximum")) {
    return 1;
  }
  return 0;
}
