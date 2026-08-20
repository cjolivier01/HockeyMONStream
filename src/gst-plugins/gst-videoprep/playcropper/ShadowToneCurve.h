#pragma once

#include <cmath>

// The two-segment shadow curve is adapted from OpenColorIO's VIDEO-style
// GradingToneTransform. Copyright Contributors to the OpenColorIO Project.
// SPDX-License-Identifier: BSD-3-Clause; see third_party/open_color_io/LICENSE.

#if defined(__CUDACC__)
#define HM_SHADOW_HOST_DEVICE __host__ __device__
#else
#define HM_SHADOW_HOST_DEVICE
#endif

namespace hm {
namespace playcropper {

constexpr float kShadowLiftMinimumPercent = 0.0f;
constexpr float kShadowLiftMaximumPercent = 100.0f;
constexpr float kShadowLiftVideoStart = 0.6f;
constexpr float kShadowLiftMaximumOcioAdjustment = 1.8f;
constexpr float kShadowLiftMaximumBlackPoint = 0.15f;

HM_SHADOW_HOST_DEVICE inline float clamp_shadow_value(float value, float minimum, float maximum) {
  return ::fminf(::fmaxf(value, minimum), maximum);
}

// Evaluates the inverse of the monotone two-piece quadratic used for OCIO's
// positive master Shadows adjustment. The endpoint slopes remain positive,
// so the curve cannot reverse tones even at the maximum supported lift. The
// optional quadratic toe raises exact black and reaches zero with zero slope at
// the protected shadow boundary.
HM_SHADOW_HOST_DEVICE inline float evaluate_shadow_lift_curve(
    float sample,
    float lift_percent,
    bool lift_black_point = false) {
  const float amount = clamp_shadow_value(lift_percent, kShadowLiftMinimumPercent, kShadowLiftMaximumPercent);
  if (amount <= 0.0f || sample < 0.0f || sample >= kShadowLiftVideoStart) {
    return sample;
  }

  float lifted = sample;
  if (sample > 0.0f) {
    constexpr float x0 = 0.0f;
    constexpr float x2 = kShadowLiftVideoStart;
    constexpr float y0 = x0;
    constexpr float y2 = x2;
    constexpr float x1 = (x0 + x2) * 0.5f;
    constexpr float m2 = 1.0f;

    const float ocio_adjustment =
        1.0f + amount * ((kShadowLiftMaximumOcioAdjustment - 1.0f) / kShadowLiftMaximumPercent);
    const float m0 = ::fmaxf(0.01f, 2.0f - ocio_adjustment);
    const float y1 = (0.5f / ((x2 - x1) + (x1 - x0))) *
        ((2.0f * y0 + m0 * (x1 - x0)) * (x2 - x1) + (2.0f * y2 - m2 * (x2 - x1)) * (x1 - x0));

    const float c_left = y0 - sample;
    const float b_left = m0 * (x1 - x0);
    const float a_left = y1 - y0 - m0 * (x1 - x0);
    const float discriminant_left = ::fmaxf(0.0f, b_left * b_left - 4.0f * a_left * c_left);
    const float t_left = (2.0f * c_left) / (-::sqrtf(discriminant_left) - b_left);
    const float out_left = t_left * (x1 - x0) + x0;

    const float c_right = y1 - sample;
    const float b_right = 2.0f * y2 - 2.0f * y1 - m2 * (x2 - x1);
    const float a_right = y1 - y2 + m2 * (x2 - x1);
    const float discriminant_right = ::fmaxf(0.0f, b_right * b_right - 4.0f * a_right * c_right);
    const float t_right = (2.0f * c_right) / (-::sqrtf(discriminant_right) - b_right);
    const float out_right = t_right * (x2 - x1) + x1;

    lifted = sample < y1 ? out_left : out_right;
  }

  if (lift_black_point) {
    const float fade = 1.0f - sample / kShadowLiftVideoStart;
    lifted += (amount / kShadowLiftMaximumPercent) * kShadowLiftMaximumBlackPoint * fade * fade;
  }
  return clamp_shadow_value(lifted, 0.0f, 1.0f);
}

} // namespace playcropper
} // namespace hm

#undef HM_SHADOW_HOST_DEVICE
