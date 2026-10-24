#pragma once

#include <gst/video/video-info.h>
#include "hstream/src/libs/gpu/vaapi/VaHipFrame.h"
#include "hstream/src/libs/program/ProgramGpu.h"

namespace hm::playcropper {
// The frame must have been imported for exclusive write. The descriptor is
// borrowed for the lifetime of that import and its explicit HIP stream.
absl::StatusOr<YuvOutput> DescribeVaProgramOutput(const gpu::VaHipFrame& frame, const GstVideoInfo& info);
} // namespace hm::playcropper
