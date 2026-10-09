#include "src/apps/hstream-ui/HighlightTracking.h"
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QStringList>
#include <algorithm>
#include <cmath>
#include "hstream/src/libs/recording/Database.h"

namespace hm::ui {
namespace {
using hm::recording::Database;
using hm::recording::Statement;
QString qstring(const std::string& s) {
  return QString::fromStdString(s);
}
std::string string(const QString& s) {
  return s.toStdString();
}
// Mirrors the recorded Program crop's rotation about its camera center. The
// Program camera and edge rotations come from the same saved frame/run.
QPointF map(
    double x,
    double y,
    double width,
    double height,
    double left,
    double top,
    double cw,
    double ch,
    double edge_left,
    double edge_right,
    const QString& route) {
  if (route == "stitched")
    return QPointF(x / width, y / height);
  const double cx = left + cw / 2, cy = top + ch / 2;
  const double angle =
      (cx < width / 2 ? edge_left * (1 - cx / (width / 2)) : edge_right * ((width / 2 - cx) / (width / 2))) * M_PI /
      180;
  const double rx = cx + (x - cx) * std::cos(angle) - (y - cy) * std::sin(angle);
  const double ry = cy + (x - cx) * std::sin(angle) + (y - cy) * std::cos(angle);
  if (route == "program-full")
    return QPointF(rx / width, ry / height);
  return QPointF((rx - left) / cw, (ry - top) / ch);
}
const char* kRows =
    "SELECT f.pts_ns/1000000,t.tracking_id,t.left,t.top,t.width,t.height,"
    "f.geometry_id,f.seek_epoch,f.reset_epoch,f.source_id,g.width,g.height,"
    "c.left,c.top,c.width,c.height,r.edge_rotation_left,r.edge_rotation_right "
    "FROM frames f JOIN tracks t USING(run_id,sample_id) JOIN geometries g USING(run_id,geometry_id) "
    "LEFT JOIN cameras c ON c.run_id=f.run_id AND c.sample_id=f.sample_id AND c.role='program' "
    "LEFT JOIN replay_frames r USING(run_id,sample_id) WHERE f.run_id=? AND f.pts_ns BETWEEN ? AND ? ";
bool point(Statement& row, const QString& route, double x, double y, QPointF* p) {
  if (route != "stitched" && (row.IsNull(12) || row.IsNull(16) || row.Real(14) <= 0 || row.Real(15) <= 0))
    return false;
  *p =
      map(x,
          y,
          row.Real(10),
          row.Real(11),
          row.Real(12),
          row.Real(13),
          row.Real(14),
          row.Real(15),
          row.Real(16),
          row.Real(17),
          route);
  return std::isfinite(p->x()) && std::isfinite(p->y());
}
} // namespace
QString HighlightTrackingDatabasePath(const QString& directory, const QString& game, const QString& archive) {
  const QDir dir(directory);
  const auto match = QRegularExpression(R"(-([0-9]+)$)").match(QFileInfo(archive).completeBaseName());
  bool valid = false;
  const uint64_t generation = match.hasMatch() ? match.captured(1).toULongLong(&valid) : 1;
  if (match.hasMatch() && !valid)
    return {};
  if (match.hasMatch()) {
    const QString literal =
        dir.filePath(qstring(hm::recording::TelemetryDatabaseStem(string(game))) + "-" + match.captured(1) + ".db");
    if (QFileInfo::exists(literal))
      return literal;
  }
  // Discovery also reads earlier version-zero recordings; the writer's
  // filename helper deliberately rejects creating those now.
  const QString expected = dir.filePath(
      qstring(hm::recording::TelemetryDatabaseStem(string(game))) + "-" + QString::number(generation) + ".db");
  if (match.hasMatch() && QFileInfo::exists(expected))
    return expected;
  const QString suffix = match.hasMatch() ? "-" + match.captured(1) : QString();
  for (const auto& prefix : {QString("hstream_telemetry"), QString("hm_telemetry")}) {
    const QString legacy = dir.filePath(prefix + suffix + ".db");
    if (QFileInfo::exists(legacy))
      return legacy;
  }
  return expected;
}
QStringList HighlightTrackingRuns(const QString& path, const QString& game, QString* error) {
  QStringList result;
  try {
    Database db(string(path));
    db.Validate();
    Statement s(db.get(), "SELECT run_id FROM runs WHERE game_id=? ORDER BY started_utc DESC");
    s.Bind(1, string(game));
    while (s.Next())
      result.append(qstring(s.Text(0)));
  } catch (const std::exception& e) {
    if (error)
      *error = QString::fromUtf8(e.what());
  }
  return result;
}
bool HighlightTrackingChoices(
    const QString& path,
    const QString& run,
    const QString& route,
    qint64 ms,
    qint64 offset,
    QVector<HighlightTrackChoice>* choices,
    QString* error) {
  choices->clear();
  try {
    const qint64 pts = ms + offset;
    if (pts < 0 || pts > 9000000000000LL)
      throw std::runtime_error("Telemetry time is outside its range");
    Database db(string(path));
    db.Validate();
    Statement s(db.get(), std::string(kRows) + "ORDER BY ABS(f.pts_ns/1000000-?),f.sample_id,t.ordinal LIMIT 2048");
    s.Bind(1, string(run));
    s.Bind(2, uint64_t(std::max<qint64>(0, pts - 100)) * 1000000);
    s.Bind(3, uint64_t(pts + 100) * 1000000);
    s.Bind(4, uint64_t(pts));
    while (s.Next()) {
      HighlightTrackChoice c;
      c.id = qstring(s.Text(1));
      c.geometry = s.Int(6);
      c.seek = s.Int(7);
      c.reset = s.Int(8);
      c.source = s.Int(9);
      bool duplicate = false;
      for (const auto& old : *choices)
        if (old.id == c.id && old.geometry == c.geometry && old.seek == c.seek && old.reset == c.reset &&
            old.source == c.source)
          duplicate = true;
      if (duplicate)
        continue;
      QPointF p0, p1, p2, p3;
      if (!point(s, route, s.Real(2), s.Real(3), &p0) ||
          !point(s, route, s.Real(2) + s.Real(4), s.Real(3) + s.Real(5), &p1) ||
          !point(s, route, s.Real(2) + s.Real(4), s.Real(3), &p2) ||
          !point(s, route, s.Real(2), s.Real(3) + s.Real(5), &p3))
        continue;
      // Rotation can put either remaining corner beyond the original diagonal.
      c.box = QRectF(
          QPointF(std::min({p0.x(), p1.x(), p2.x(), p3.x()}), std::min({p0.y(), p1.y(), p2.y(), p3.y()})),
          QPointF(std::max({p0.x(), p1.x(), p2.x(), p3.x()}), std::max({p0.y(), p1.y(), p2.y(), p3.y()})));
      choices->append(c);
    }
    if (choices->isEmpty())
      throw std::runtime_error(
          "No usable tracks at this time. Check the run, time binding and archive geometry, or use manual keyframes.");
    return true;
  } catch (const std::exception& e) {
    if (error)
      *error = QString::fromUtf8(e.what());
    return false;
  }
}
bool CaptureHighlightTrack(
    HighlightAnnotation* a,
    const HighlightTrackChoice& c,
    const QString& route,
    QString* error) {
  try {
    const qint64 start = a->start_ms + a->telemetry_offset_ms, end = a->end_ms + a->telemetry_offset_ms;
    if (start < 0 || end <= start || end > 9000000000000LL || end - start > 120000)
      throw std::runtime_error("A tracked cue must fit a positive telemetry range of at most two minutes");
    Database db(string(a->database));
    db.Validate();
    Statement s(
        db.get(),
        std::string(kRows) +
            "AND t.tracking_id=? AND f.geometry_id=? AND f.seek_epoch=? AND f.reset_epoch=? AND f.source_id=? ORDER BY f.pts_ns,f.sample_id LIMIT 4001");
    s.Bind(1, string(a->run_id));
    s.Bind(2, uint64_t(start) * 1000000);
    s.Bind(3, uint64_t(end) * 1000000);
    s.Bind(4, string(c.id));
    s.Bind(5, uint64_t(c.geometry));
    s.Bind(6, uint64_t(c.seek));
    s.Bind(7, uint64_t(c.reset));
    s.Bind(8, uint64_t(c.source));
    QVector<HighlightPosition> positions;
    while (s.Next()) {
      QPointF p;
      if (!point(s, route, s.Real(2) + s.Real(4) / 2, s.Real(3), &p))
        continue;
      const qint64 t = s.Int(0) - a->telemetry_offset_ms;
      if (!positions.isEmpty() && t <= positions.last().time_ms)
        throw std::runtime_error("Ambiguous telemetry timestamps in this segment");
      if (std::abs(p.x()) > 2 || std::abs(p.y()) > 2)
        continue;
      positions.append({t, p.x(), p.y()});
    }
    if (positions.isEmpty() || positions.size() > 4000)
      throw std::runtime_error("Track has no positions or exceeds 4000 observations");
    a->positions = positions;
    a->track_id = c.id;
    a->geometry_id = c.geometry;
    a->seek_epoch = c.seek;
    a->reset_epoch = c.reset;
    a->source_id = c.source;
    a->motion = "track";
    a->x = 0;
    a->y = -0.12;
    return ValidateHighlightAnnotation(*a, error);
  } catch (const std::exception& e) {
    if (error)
      *error = QString::fromUtf8(e.what());
    return false;
  }
}
} // namespace hm::ui
