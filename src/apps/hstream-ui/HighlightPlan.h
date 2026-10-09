#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

namespace hm::ui {

// Positions and dimensions are fractions of the output image. Annotation times
// are absolute game/source times, so trimming and reel edits preserve events.
struct HighlightPosition {
  qint64 time_ms{0};
  double x{0.5}, y{0.5};
};

struct HighlightAnnotation {
  QString kind{"arrow"}; // arrow, text, box
  QString text;
  qint64 start_ms{0}, end_ms{2000};
  double x{0.5}, y{0.35}, dx{0}, dy{0.12};
  double size{0.055}, thickness{0.008};
  int weight{900};
  QString color{"#ffff00"};
  bool blink{true};
  qint64 blink_ms{250}; // each on/off phase
  QString motion{"fixed"}; // fixed, keyframes, track
  QVector<HighlightPosition> positions;
  // Explicit recorded identity. Samples are retained with the plan so preview
  // and export do not depend on a mutable database or new runtime track IDs.
  QString database, run_id, track_id, archive_path;
  qint64 geometry_id{0}, seek_epoch{0}, reset_epoch{0}, source_id{0};
  qint64 telemetry_offset_ms{0};
  qint64 archive_origin_ms{0}, archive_size{0}, archive_mtime_ms{0};
};

struct HighlightCard {
  bool matchup{false};
  QString heading{"Game Highlights"};
  QString team_a, team_b, logo_a, logo_b, date;
  qint64 duration_ms{3000};
  QString background{"#101b2c"}, color{"#ffffff"};
  double size{0.075};
  int weight{800};
};

struct HighlightInterval {
  QString label;
  bool event_mode{true};
  qint64 event_ms{0};
  qint64 duration_ms{0};
  qint64 start_ms{0};
  qint64 end_ms{0};
  bool is_card{false};
  HighlightCard card;
  QVector<HighlightAnnotation> annotations;
};

struct HighlightPlan {
  QVector<HighlightInterval> intervals;
  QString base_name{"highlights"};
  QString archive_path;
};

// Times use the recording's logical timeline. The upper bound keeps milliseconds
// convertible to a signed nanosecond timestamp for the pipeline.
bool ParseHighlightTime(const QString& text, qint64* milliseconds, QString* error = nullptr);
QString FormatHighlightTime(qint64 milliseconds);
bool NormalizeHighlightInterval(HighlightInterval* interval, QString* error = nullptr);
bool ValidateHighlightAnnotation(const HighlightAnnotation& annotation, QString* error = nullptr);
qint64 HighlightItemDuration(const HighlightInterval& item);
bool LoadHighlightPlan(const QString& path, HighlightPlan* plan, QString* error = nullptr);
bool SaveHighlightPlan(const QString& path, const HighlightPlan& plan, QString* error = nullptr);

} // namespace hm::ui
