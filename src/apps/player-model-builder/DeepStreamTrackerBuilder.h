#pragma once

#include <string>

// Run only in the short-lived preparation process. DeepStream owns decoding
// its supplied TAO model and constructing its compatible TensorRT engine.
void BuildDeepStreamTracker(const std::string& config, const std::string& library, int gpu_id);
