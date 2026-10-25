#include "PreviewOverlayGlPaths.h"

#include <GL/gl.h>

#include <algorithm>

namespace hm::gpu_preview {

void DrawPreviewOverlayGlPaths(const PreviewOverlayScene& overlays) {
  if (overlays.coordinate_width <= 0.0F || overlays.coordinate_height <= 0.0F)
    return;
  glDisable(GL_TEXTURE_2D);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  for (const OverlayPath& path : overlays.paths) {
    if (path.points.size() < (path.filled ? 3U : 2U))
      continue;
    glColor4f(path.color.red, path.color.green, path.color.blue, path.color.alpha);
    glLineWidth(std::max(1.0F, path.width));
    glBegin(path.filled ? GL_POLYGON : (path.closed ? GL_LINE_LOOP : GL_LINE_STRIP));
    for (const auto& point : path.points) {
      glVertex2f(
          -1.0F + 2.0F * point.x / overlays.coordinate_width, 1.0F - 2.0F * point.y / overlays.coordinate_height);
    }
    glEnd();
  }
  glLineWidth(1.0F);
  glColor4f(1.0F, 1.0F, 1.0F, 1.0F);
}

} // namespace hm::gpu_preview
