#pragma once

#include "src/apps/hstream-ui/CameraExperimentPreviewOutput.h"
#include "src/apps/hstream-ui/HighlightWebPreview.h"

// The source graph may pause or rebuild independently of this live transport.
// One retained NVMM frame is repeated with a separate presentation clock. A
// receipt still names the exact original source PTS and replay generation.
std::shared_ptr<CameraExperimentPreviewOutput> MakeCameraExperimentWebPreviewOutput(
    const hm::ui::HighlightWebPreviewConfig& config,
    std::string* error);
