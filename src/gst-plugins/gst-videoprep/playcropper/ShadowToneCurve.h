#pragma once

#include <cmath>

#if defined(__CUDACC__)
#define HM_SHADOW_HOST_DEVICE __host__ __device__
#else
#define HM_SHADOW_HOST_DEVICE
#endif

namespace hm {
namespace playcropper {

constexpr float kShadowLiftMinimumPercent = 0.0f;
constexpr float kShadowLiftMaximumPercent = 100.0f;
// A full lift uses the gamma fitted to m1p_lifted_shadows.png. Applying the
// exponent to Rec. 709 luma and scaling RGB together was the closest simple,
// GPU-friendly match to the supplied image while preserving hue.
constexpr float kShadowLiftReferenceGamma = 0.812f;
constexpr float kShadowLiftLumaRed = 0.2126f;
constexpr float kShadowLiftLumaGreen = 0.7152f;
constexpr float kShadowLiftLumaBlue = 0.0722f;
constexpr float kShadowLiftBlackPointFadeEnd = 0.6f;
constexpr float kShadowLiftMaximumBlackPoint = 0.15f;
constexpr float kExposureMinimumSetting = 0.0f;
constexpr float kExposureMaximumSetting = 1.3f;
constexpr float kExposureStopScale = 0.5f;

HM_SHADOW_HOST_DEVICE inline float clamp_shadow_value(float value, float minimum, float maximum) {
  return ::fminf(::fmaxf(value, minimum), maximum);
}

// Pixel pairs from the supplied reference exports at exposure settings 0.3,
// 0.6, 1.0, and 1.3 follow this channel-independent gain to within one 8-bit
// code value:
//
//   output = clamp(input * 2^(setting / 2), 0, 1)
//
// This is a uniform exposure-style gain: it preserves exact black, raises all
// nonzero tones, and clips at white. Keep it independent from the shadow and
// midtone lift below so either grade remains byte-for-byte disabled at zero.
HM_SHADOW_HOST_DEVICE inline float exposure_gain(float setting) {
  const float amount = clamp_shadow_value(setting, kExposureMinimumSetting, kExposureMaximumSetting);
  return ::exp2f(amount * kExposureStopScale);
}

HM_SHADOW_HOST_DEVICE inline float evaluate_exposure(float sample, float setting) {
  return clamp_shadow_value(sample * exposure_gain(setting), 0.0f, 1.0f);
}

HM_SHADOW_HOST_DEVICE inline float shadow_lift_amount(float lift_percent) {
  return clamp_shadow_value(lift_percent, kShadowLiftMinimumPercent, kShadowLiftMaximumPercent) /
      kShadowLiftMaximumPercent;
}

// Interpolate strength in exponent space. This keeps every nonzero setting a
// conventional gamma lift and reaches the fitted reference gamma at 100%.
HM_SHADOW_HOST_DEVICE inline float shadow_lift_gamma(float lift_percent) {
  return ::powf(kShadowLiftReferenceGamma, shadow_lift_amount(lift_percent));
}

HM_SHADOW_HOST_DEVICE inline float shadow_black_point_offset(float input_luma, float amount) {
  if (amount <= 0.0f || input_luma < 0.0f || input_luma >= kShadowLiftBlackPointFadeEnd) {
    return 0.0f;
  }
  const float fade = 1.0f - input_luma / kShadowLiftBlackPointFadeEnd;
  return amount * kShadowLiftMaximumBlackPoint * fade * fade;
}

// Lift perceptual Rec. 709 luma with a gamma-like curve. This raises shadows
// and midtones, naturally rolls back toward exact white, and preserves exact
// black unless the separately requested black-point toe is enabled.
HM_SHADOW_HOST_DEVICE inline float evaluate_shadow_lift_luma(
    float sample,
    float gamma,
    float amount,
    bool lift_black_point = false) {
  if (amount <= 0.0f || sample < 0.0f || sample > 1.0f) {
    return sample;
  }
  const float lifted = sample > 0.0f && sample < 1.0f ? ::powf(sample, gamma) : sample;
  const float black_point = lift_black_point ? shadow_black_point_offset(sample, amount) : 0.0f;
  return clamp_shadow_value(lifted + black_point, 0.0f, 1.0f);
}

HM_SHADOW_HOST_DEVICE inline float evaluate_shadow_lift_luma(
    float sample,
    float lift_percent,
    bool lift_black_point = false) {
  return evaluate_shadow_lift_luma(
      sample, shadow_lift_gamma(lift_percent), shadow_lift_amount(lift_percent), lift_black_point);
}

// Scale all three channels from the same lifted luma so chroma ratios and hue
// remain stable. The optional toe is neutral and intentionally desaturates only
// the deepest shadows as it raises the black point.
HM_SHADOW_HOST_DEVICE inline void evaluate_shadow_lift_rgb(
    float* red,
    float* green,
    float* blue,
    float gamma,
    float amount,
    bool lift_black_point = false) {
  if (!red || !green || !blue || amount <= 0.0f) {
    return;
  }
  const float input_luma = *red * kShadowLiftLumaRed + *green * kShadowLiftLumaGreen + *blue * kShadowLiftLumaBlue;
  const float lifted_luma = input_luma > 0.0f && input_luma < 1.0f ? ::powf(input_luma, gamma) : input_luma;
  const float scale = input_luma > 0.0f ? lifted_luma / input_luma : 1.0f;
  const float black_point = lift_black_point ? shadow_black_point_offset(input_luma, amount) : 0.0f;
  *red = clamp_shadow_value(*red * scale + black_point, 0.0f, 1.0f);
  *green = clamp_shadow_value(*green * scale + black_point, 0.0f, 1.0f);
  *blue = clamp_shadow_value(*blue * scale + black_point, 0.0f, 1.0f);
}

} // namespace playcropper
} // namespace hm

#undef HM_SHADOW_HOST_DEVICE
