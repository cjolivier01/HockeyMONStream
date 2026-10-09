#pragma once

#include <QtCore/QPointF>
#include <QtGui/QImage>
#include <optional>
#include "src/apps/hstream-ui/HighlightPlan.h"

namespace hm::ui {
// CPU rasterization is limited to static authored assets, never decoded video.
struct HighlightTexture {
  QImage image;
  QPointF origin; // relative to the annotation anchor, in image fractions
};
std::optional<QPointF> HighlightAnchor(const HighlightAnnotation& annotation, qint64 game_ms);
HighlightTexture RasterHighlightAnnotation(const HighlightAnnotation& annotation);
bool RasterHighlightCard(const HighlightCard& card, const QString& asset_root, QImage* image, QString* error);
bool ImportHighlightLogo(const QString& source, const QString& game_dir, QString* relative_path, QString* error);
} // namespace hm::ui
