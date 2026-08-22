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
  using hm::playcropper::evaluate_exposure;
  using hm::playcropper::evaluate_shadow_lift_luma;
  constexpr float kTolerance = 2e-5f;

  if (!expect(evaluate_shadow_lift_luma(0.0f, 100.0f) == 0.0f, "Shadow lift must preserve exact black") ||
      !expect(evaluate_shadow_lift_luma(1.0f, 100.0f) == 1.0f, "Shadow lift must preserve exact white")) {
    return 1;
  }

  for (int sample = 0; sample <= 10000; ++sample) {
    const float value = static_cast<float>(sample) / 10000.0f;
    if (!expect(
            std::abs(evaluate_shadow_lift_luma(value, 0.0f) - value) <= kTolerance,
            "A zero shadow lift must be a pixel-exact identity")) {
      return 1;
    }
  }

  for (bool lift_black_point : {false, true}) {
    for (float amount : std::array<float, 4>{25.0f, 50.0f, 75.0f, 100.0f}) {
      float previous = evaluate_shadow_lift_luma(0.0f, amount, lift_black_point);
      for (int sample = 1; sample <= 65535; ++sample) {
        const float value = static_cast<float>(sample) / 65535.0f;
        const float lifted = evaluate_shadow_lift_luma(value, amount, lift_black_point);
        if (!expect(std::isfinite(lifted), "Shadow lift must remain finite across the full signal range") ||
            !expect(lifted + kTolerance >= previous, "Shadow lift must not introduce a tone reversal") ||
            !expect(lifted + kTolerance >= value, "A positive shadow lift must not darken the input") ||
            !expect(lifted <= 1.0f + kTolerance, "Shadow lift must remain inside the video signal range")) {
          return 1;
        }
        previous = lifted;
      }
    }
  }

  // Gamma 0.812 is the best simple Rec. 709-luma fit to the supplied lifted
  // reference. It raises midtones as well as shadows and rolls off at white.
  const std::array<float, 7> reference_inputs = {0.05f, 0.1f, 0.2f, 0.4f, 0.6f, 0.75f, 0.9f};
  const std::array<float, 7> reference_expected = {
      0.087813976f, 0.154170045f, 0.270667653f, 0.475195931f, 0.660478698f, 0.791680132f, 0.918004727f};
  for (size_t i = 0; i < reference_inputs.size(); ++i) {
    if (!expect(
            std::abs(evaluate_shadow_lift_luma(reference_inputs[i], 100.0f) - reference_expected[i]) <= kTolerance,
            "Full shadow lift must match the fitted reference gamma")) {
      return 1;
    }
  }

  const float dark_sample = 0.1f;
  if (!expect(
          evaluate_shadow_lift_luma(dark_sample, 100.0f) > evaluate_shadow_lift_luma(dark_sample, 50.0f) &&
              evaluate_shadow_lift_luma(dark_sample, 50.0f) > dark_sample,
          "Increasing the slider must progressively reveal more shadow detail") ||
      !expect(
          evaluate_shadow_lift_luma(dark_sample, -10.0f) == dark_sample,
          "Out-of-range negative input must clamp to identity") ||
      !expect(
          std::abs(evaluate_shadow_lift_luma(dark_sample, 150.0f) - evaluate_shadow_lift_luma(dark_sample, 100.0f)) <=
              kTolerance,
          "Out-of-range positive input must clamp to the validated maximum")) {
    return 1;
  }
  if (!expect(
          evaluate_shadow_lift_luma(0.0f, 0.0f, true) == 0.0f,
          "The black-point toggle must remain an identity when shadow lift is zero") ||
      !expect(
          std::abs(evaluate_shadow_lift_luma(0.0f, 100.0f, true) - hm::playcropper::kShadowLiftMaximumBlackPoint) <=
              kTolerance,
          "The black-point toggle must reach its documented maximum at full lift") ||
      !expect(
          evaluate_shadow_lift_luma(dark_sample, 100.0f, true) > evaluate_shadow_lift_luma(dark_sample, 100.0f, false),
          "The black-point toggle must visibly strengthen deep-shadow recovery") ||
      !expect(
          evaluate_shadow_lift_luma(hm::playcropper::kShadowLiftBlackPointFadeEnd, 100.0f, true) ==
              evaluate_shadow_lift_luma(hm::playcropper::kShadowLiftBlackPointFadeEnd, 100.0f, false),
          "The optional black-point toe must fade out by 60% luma")) {
    return 1;
  }

  float red = 0.1f;
  float green = 0.25f;
  float blue = 0.5f;
  const float input_luma = red * hm::playcropper::kShadowLiftLumaRed + green * hm::playcropper::kShadowLiftLumaGreen +
      blue * hm::playcropper::kShadowLiftLumaBlue;
  const float red_green_ratio = red / green;
  const float blue_green_ratio = blue / green;
  hm::playcropper::evaluate_shadow_lift_rgb(
      &red, &green, &blue, hm::playcropper::shadow_lift_gamma(100.0f), hm::playcropper::shadow_lift_amount(100.0f));
  const float output_luma = red * hm::playcropper::kShadowLiftLumaRed + green * hm::playcropper::kShadowLiftLumaGreen +
      blue * hm::playcropper::kShadowLiftLumaBlue;
  if (!expect(
          std::abs(output_luma - evaluate_shadow_lift_luma(input_luma, 100.0f)) <= kTolerance,
          "RGB shadow lift must apply the fitted gamma to Rec. 709 luma") ||
      !expect(
          std::abs(red / green - red_green_ratio) <= kTolerance &&
              std::abs(blue / green - blue_green_ratio) <= kTolerance,
          "RGB shadow lift must preserve channel ratios when no channel clips")) {
    return 1;
  }

  red = green = blue = 0.0f;
  hm::playcropper::evaluate_shadow_lift_rgb(
      &red,
      &green,
      &blue,
      hm::playcropper::shadow_lift_gamma(100.0f),
      hm::playcropper::shadow_lift_amount(100.0f),
      true);
  if (!expect(
          std::abs(red - hm::playcropper::kShadowLiftMaximumBlackPoint) <= kTolerance && red == green && green == blue,
          "The optional RGB black-point toe must raise exact black neutrally")) {
    return 1;
  }

  struct ExposureReference {
    float setting;
    std::array<int, 5> outputs;
  };
  constexpr std::array<int, 5> exposure_inputs = {16, 32, 64, 128, 160};
  // Per-code medians measured from the aligned reference PNG exports.
  constexpr std::array<ExposureReference, 4> exposure_references = {{
      {0.3f, {18, 36, 71, 142, 178}},
      {0.6f, {20, 39, 79, 158, 197}},
      {1.0f, {23, 45, 91, 181, 226}},
      {1.3f, {25, 50, 100, 201, 251}},
  }};
  for (const ExposureReference& reference : exposure_references) {
    for (size_t i = 0; i < exposure_inputs.size(); ++i) {
      const int output =
          static_cast<int>(evaluate_exposure(exposure_inputs[i] / 255.0f, reference.setting) * 255.0f + 0.5f);
      if (!expect(
              std::abs(output - reference.outputs[i]) <= 1,
              "Exposure must reproduce the measured reference pixels within one 8-bit code value")) {
        return 1;
      }
    }
  }
  if (!expect(evaluate_exposure(0.0f, 1.3f) == 0.0f, "Exposure must preserve exact black") ||
      !expect(evaluate_exposure(1.0f, 1.3f) == 1.0f, "Exposure must clip at white") ||
      !expect(evaluate_exposure(0.5f, 0.0f) == 0.5f, "Zero exposure must preserve legacy pixels exactly") ||
      !expect(
          std::abs(hm::playcropper::exposure_gain(1.0f) - std::sqrt(2.0f)) <= kTolerance,
          "Exposure setting 1.0 must apply the measured square-root-of-two gain")) {
    return 1;
  }
  return 0;
}
