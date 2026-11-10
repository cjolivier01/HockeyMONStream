#pragma once

#include <gst/gst.h>
#include "absl/status/status.h"
#include "hstream/src/libs/detection/TileInference.h"

namespace hm::pipeline {
// Configure the existing nvinfer as a synchronous ROI detector. Its GPU crop
// transform submits both temporary regions in one batch; the src probe removes
// all temporary metadata and merges detections before downstream observers.
absl::Status InstallTiledInference(
    GstElement* infer,
    const char* inference_file,
    const detection::TileInferenceSettings& settings);

// Metadata boundaries, also exercised without a GPU by regression tests.
absl::Status PrepareTiledInferenceBuffer(GstBuffer* buffer, const detection::TileInferenceSettings& settings);
absl::Status MergeTiledInferenceBuffer(GstBuffer* buffer, const detection::TileInferenceSettings& settings);
} // namespace hm::pipeline
