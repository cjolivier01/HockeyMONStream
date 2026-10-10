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
  QSize reference_size{1920, 1080};
};
std::optional<QPointF> HighlightAnchor(const HighlightAnnotation& annotation, qint64 game_ms);
HighlightTexture RasterHighlightAnnotation(
    const HighlightAnnotation& annotation,
    QSize output_size = QSize(1920, 1080));
bool RasterHighlightCard(
    const HighlightCard& card,
    const QString& asset_root,
    QImage* image,
    QString* error,
    QSize output_size = QSize(1920, 1080));
// An editor may keep already-broken artwork, but must report a failed attempt
// to retain readable artwork instead of silently keeping an external path.
enum class HighlightLogoImportResult { Imported, UnreadableSource, StorageFailure };
HighlightLogoImportResult ImportHighlightLogo(
    const QString& source,
    const QString& game_dir,
    QString* relative_path,
    QString* error);
} // namespace hm::ui
