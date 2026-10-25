#pragma once

#include "hstream/src/apps/apps-common/PreviewOverlayScene.h"

namespace hm::gpu_preview {

// The native desktop diagnostic path pass, shared verbatim with offscreen
// preview rendering. Requires a current compatibility GL context and the video
// viewport; coordinates, aliased display-pixel widths and blend order are kept.
void DrawPreviewOverlayGlPaths(const PreviewOverlayScene& overlays);

} // namespace hm::gpu_preview
