#pragma once

#include <gst/video/video.h>

#include "absl/status/statusor.h"
#include "hstream/src/libs/detection/DetectorGpu.h"
#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"

namespace hm::detection {
// Describe retained VA/HIP planes using negotiated colorimetry. No fallback
// matrix/range, pixel access, or lifetime extension. HDR needs explicit tone
// mapping before the SDR detector and is rejected here.
absl::StatusOr<Image> DescribeVaDetectorImage(const gpu::VaHipFrame& frame, const GstVideoInfo& info);
} // namespace hm::detection
