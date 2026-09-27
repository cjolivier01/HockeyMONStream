#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

namespace hm::ui {

struct HighlightInterval {
  QString label;
  bool event_mode{true};
  qint64 event_ms{0};
  qint64 duration_ms{0};
  qint64 start_ms{0};
  qint64 end_ms{0};
};

struct HighlightPlan {
  QVector<HighlightInterval> intervals;
  QString base_name{"highlights"};
};

// Times use the recording's logical timeline. The upper bound keeps milliseconds
// convertible to a signed nanosecond timestamp for the pipeline.
bool ParseHighlightTime(const QString& text, qint64* milliseconds, QString* error = nullptr);
QString FormatHighlightTime(qint64 milliseconds);
bool NormalizeHighlightInterval(HighlightInterval* interval, QString* error = nullptr);
bool LoadHighlightPlan(const QString& path, HighlightPlan* plan, QString* error = nullptr);
bool SaveHighlightPlan(const QString& path, const HighlightPlan& plan, QString* error = nullptr);

} // namespace hm::ui
