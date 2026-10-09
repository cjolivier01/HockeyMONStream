#pragma once
#include <QtCore/QRectF>
#include <QtCore/QStringList>
#include "src/apps/hstream-ui/HighlightPlan.h"

namespace hm::ui {
struct HighlightTrackChoice {
  QString id;
  qint64 geometry{0}, seek{0}, reset{0}, source{0};
  QRectF box;
};
QStringList HighlightTrackingRuns(const QString& database, const QString& game, QString* error);
bool HighlightTrackingChoices(
    const QString& database,
    const QString& run,
    const QString& route,
    qint64 game_ms,
    qint64 pts_offset_ms,
    QVector<HighlightTrackChoice>* choices,
    QString* error);
bool CaptureHighlightTrack(
    HighlightAnnotation* annotation,
    const HighlightTrackChoice& choice,
    const QString& route,
    QString* error);
} // namespace hm::ui
