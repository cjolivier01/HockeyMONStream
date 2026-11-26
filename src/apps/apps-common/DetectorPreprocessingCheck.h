#pragma once

#include "absl/status/status.h"

namespace hm::pipeline {

// Exercise DeepStream's installed RGB-to-planar conversion before playback.
// The check owns its CUDA stream and test pixels; it never takes a video buffer.
absl::Status CheckDetectorPreprocessing(unsigned gpu_id);

} // namespace hm::pipeline
