#pragma once

#include <cuda_runtime_api.h>

#include <cstddef>
#include <cstdint>

#include "absl/status/statusor.h"
#include "hstream/src/libs/player_analytics/Types.h"

namespace hm::player_analytics {

inline constexpr int kPoseInputWidth = 192;
inline constexpr int kPoseInputHeight = 256;
inline constexpr size_t kPoseInputElements = 3 * kPoseInputWidth * kPoseInputHeight;

enum class PixelFormat { kRgba8, kRgba10A2 };

struct ImageView {
  const void* data{nullptr};
  size_t pitch{0};
  int width{0};
  int height{0};
  // Packed RGB10 has R in bits 0..9, G 10..19, B 20..29; alpha is ignored.
  PixelFormat format{PixelFormat::kRgba8};
};

struct PoseRoi {
  Box metadata_box;
  float metadata_width{0};
  float metadata_height{0};
};

struct PoseAffine {
  // Inverse of the float32 affine used by MMPose's non-UDP topdown path.
  // Model pixel (u,v) samples surface pixel (x0 + u*dx, y0 + v*dy).
  double x0{0};
  double y0{0};
  double dx{0};
  double dy{0};
  float surface_to_metadata_x{0};
  float surface_to_metadata_y{0};
};

// Pure CPU metadata arithmetic; never touches frame pixels. Expands 1.25 then
// fixes 192:256 aspect in surface pixels. Out-of-frame ROIs retain their affine.
absl::StatusOr<PoseAffine> MakePoseAffine(const ImageView& image, const PoseRoi& roi);

// Borrowed GPU-only input. The caller uploads the bounded affine array and
// owns all buffer lifetimes through stream completion. No allocation or sync.
// Implements OpenCV INTER_LINEAR fixed-point sampling, constant-zero border,
// uint8 rounding, RGB ImageNet normalization, contiguous float32 NCHW output.
cudaError_t PreprocessPose(
    const ImageView& image,
    const PoseAffine* device_affines,
    size_t batch,
    float* device_input,
    cudaStream_t stream);

// Reduces full SimCC distributions on GPU; only batch*17 keypoints need D2H.
// First argmax wins ties, split ratio=2, confidence=min(x_max,y_max) clamped
// to [0,1]. Nonfinite/nonpositive joints produce a zero-confidence keypoint.
cudaError_t DecodePose(
    const float* simcc_x,
    const float* simcc_y,
    const PoseAffine* device_affines,
    size_t batch,
    Pose* device_results,
    cudaStream_t stream);

} // namespace hm::player_analytics
