#include "src/apps/hstream-ui/HighlightPlan.h"

#include <QtCore/QDate>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>

namespace hm::ui {
namespace {

constexpr qint64 kNanosecondsPerMillisecond = 1000000;
constexpr qint64 kMaximumMilliseconds = std::numeric_limits<qint64>::max() / kNanosecondsPerMillisecond;

bool fail(QString* error, const QString& message) {
  if (error)
    *error = message;
  return false;
}

bool valid_milliseconds(qint64 value) {
  return value >= 0 && value <= kMaximumMilliseconds;
}

bool read_milliseconds(const QJsonObject& object, const QString& key, qint64* value, QString* error) {
  const QJsonValue json_value = object.value(key);
  if (!json_value.isDouble())
    return fail(error, QString("%1 must be an integer number of milliseconds").arg(key));
  const double number = json_value.toDouble();
  if (number < 0 || number > static_cast<double>(kMaximumMilliseconds) ||
      number != static_cast<double>(static_cast<qint64>(number))) {
    return fail(error, QString("%1 must be a nonnegative millisecond value within the pipeline time range").arg(key));
  }
  *value = static_cast<qint64>(number);
  return true;
}

bool valid_base_name(const QString& name) {
  static const QRegularExpression pattern("^[A-Za-z0-9][A-Za-z0-9 _-]{0,63}$");
  return pattern.match(name).hasMatch();
}

bool valid_color(const QString& color) {
  static const QRegularExpression pattern("^#[0-9a-fA-F]{6}$");
  return pattern.match(color).hasMatch();
}

QJsonObject annotation_json(const HighlightAnnotation& a) {
  QJsonArray positions;
  for (const auto& p : a.positions)
    positions.append(QJsonObject{{"time_ms", p.time_ms}, {"x", p.x}, {"y", p.y}});
  return {
      {"kind", a.kind},
      {"text", a.text},
      {"start_ms", a.start_ms},
      {"end_ms", a.end_ms},
      {"x", a.x},
      {"y", a.y},
      {"dx", a.dx},
      {"dy", a.dy},
      {"size", a.size},
      {"thickness", a.thickness},
      {"weight", a.weight},
      {"color", a.color},
      {"blink", a.blink},
      {"blink_ms", a.blink_ms},
      {"motion", a.motion},
      {"positions", positions},
      {"database", a.database},
      {"run_id", a.run_id},
      {"track_id", a.track_id},
      {"archive_path", a.archive_path},
      {"archive_origin_ms", a.archive_origin_ms},
      {"archive_size", a.archive_size},
      {"archive_mtime_ms", a.archive_mtime_ms},
      {"geometry_id", a.geometry_id},
      {"seek_epoch", a.seek_epoch},
      {"reset_epoch", a.reset_epoch},
      {"source_id", a.source_id},
      {"telemetry_offset_ms", a.telemetry_offset_ms}};
}

bool read_annotation(const QJsonObject& o, HighlightAnnotation* a, QString* error) {
  for (const auto& key : {"kind", "text", "color", "motion", "database", "run_id", "track_id", "archive_path"})
    if (!o.value(key).isString())
      return fail(error, QString("Annotation %1 must be a string").arg(key));
  for (const auto& key : {"x", "y", "dx", "dy", "size", "thickness", "weight", "telemetry_offset_ms"})
    if (!o.value(key).isDouble())
      return fail(error, QString("Annotation %1 must be a number").arg(key));
  if (!o.value("blink").isBool() || !o.value("positions").isArray())
    return fail(error, "Invalid annotation blink or positions");
  a->kind = o.value("kind").toString();
  a->text = o.value("text").toString();
  a->color = o.value("color").toString();
  a->motion = o.value("motion").toString();
  a->database = o.value("database").toString();
  a->run_id = o.value("run_id").toString();
  a->track_id = o.value("track_id").toString();
  a->archive_path = o.value("archive_path").toString();
  a->x = o.value("x").toDouble();
  a->y = o.value("y").toDouble();
  a->dx = o.value("dx").toDouble();
  a->dy = o.value("dy").toDouble();
  a->size = o.value("size").toDouble();
  a->thickness = o.value("thickness").toDouble();
  a->weight = o.value("weight").toInt(-1);
  a->blink = o.value("blink").toBool();
  const double offset = o.value("telemetry_offset_ms").toDouble();
  if (!std::isfinite(offset) || std::abs(offset) > kMaximumMilliseconds || std::trunc(offset) != offset)
    return fail(error, "Invalid telemetry time offset");
  a->telemetry_offset_ms = static_cast<qint64>(offset);
  if (!read_milliseconds(o, "start_ms", &a->start_ms, error) || !read_milliseconds(o, "end_ms", &a->end_ms, error) ||
      !read_milliseconds(o, "blink_ms", &a->blink_ms, error) ||
      !read_milliseconds(o, "geometry_id", &a->geometry_id, error) ||
      !read_milliseconds(o, "seek_epoch", &a->seek_epoch, error) ||
      !read_milliseconds(o, "reset_epoch", &a->reset_epoch, error) ||
      !read_milliseconds(o, "source_id", &a->source_id, error) ||
      !read_milliseconds(o, "archive_origin_ms", &a->archive_origin_ms, error) ||
      !read_milliseconds(o, "archive_size", &a->archive_size, error) ||
      !read_milliseconds(o, "archive_mtime_ms", &a->archive_mtime_ms, error))
    return false;
  const auto positions = o.value("positions").toArray();
  if (positions.size() > 4000)
    return fail(error, "Too many annotation positions (maximum 4000)");
  for (const auto& v : positions) {
    if (!v.isObject())
      return fail(error, "Invalid annotation position");
    const auto p = v.toObject();
    HighlightPosition point;
    if (!read_milliseconds(p, "time_ms", &point.time_ms, error) || !p.value("x").isDouble() || !p.value("y").isDouble())
      return false;
    point.x = p.value("x").toDouble();
    point.y = p.value("y").toDouble();
    a->positions.append(point);
  }
  return ValidateHighlightAnnotation(*a, error);
}

QJsonObject card_json(const HighlightCard& c) {
  return {
      {"matchup", c.matchup},
      {"heading", c.heading},
      {"team_a", c.team_a},
      {"team_b", c.team_b},
      {"logo_a", c.logo_a},
      {"logo_b", c.logo_b},
      {"date", c.date},
      {"duration_ms", c.duration_ms},
      {"background", c.background},
      {"color", c.color},
      {"size", c.size},
      {"weight", c.weight}};
}

bool read_card(const QJsonObject& o, HighlightCard* c, QString* error) {
  for (const auto& key : {"heading", "team_a", "team_b", "logo_a", "logo_b", "date", "background", "color"})
    if (!o.value(key).isString())
      return fail(error, QString("Card %1 must be a string").arg(key));
  if (!o.value("matchup").isBool() || !o.value("size").isDouble() || !o.value("weight").isDouble())
    return fail(error, "Invalid card style");
  c->matchup = o.value("matchup").toBool();
  c->heading = o.value("heading").toString();
  c->team_a = o.value("team_a").toString();
  c->team_b = o.value("team_b").toString();
  c->logo_a = o.value("logo_a").toString();
  c->logo_b = o.value("logo_b").toString();
  c->date = o.value("date").toString();
  c->background = o.value("background").toString();
  c->color = o.value("color").toString();
  c->size = o.value("size").toDouble();
  c->weight = o.value("weight").toInt(-1);
  return read_milliseconds(o, "duration_ms", &c->duration_ms, error);
}

} // namespace

bool ParseHighlightTime(const QString& text, qint64* milliseconds, QString* error) {
  if (!milliseconds)
    return fail(error, "No destination was supplied for the highlight time");
  static const QRegularExpression pattern("^([0-9]+)(?::([0-9]+))?(?::([0-9]+))?(?:\\.([0-9]{1,3}))?$");
  const QRegularExpressionMatch match = pattern.match(text.trimmed());
  if (!match.hasMatch())
    return fail(error, "Use seconds, MM:SS, or HH:MM:SS, optionally followed by .mmm");

  bool first_ok = false;
  bool second_ok = false;
  bool third_ok = false;
  const qint64 first = match.captured(1).toLongLong(&first_ok);
  const bool has_second = match.capturedStart(2) >= 0;
  const bool has_third = match.capturedStart(3) >= 0;
  const qint64 second = has_second ? match.captured(2).toLongLong(&second_ok) : 0;
  const qint64 third = has_third ? match.captured(3).toLongLong(&third_ok) : 0;
  if (!first_ok || (has_second && !second_ok) || (has_third && !third_ok))
    return fail(error, "Highlight time exceeds the pipeline time range");
  const qint64 hours = has_third ? first : 0;
  const qint64 minutes = has_third ? second : has_second ? first : 0;
  const qint64 seconds = has_third ? third : has_second ? second : first;
  if ((has_second && seconds >= 60) || (has_third && minutes >= 60))
    return fail(error, "Seconds must be below 60 after a colon, and HH:MM:SS minutes must be below 60");
  QString fraction = match.captured(4);
  while (fraction.size() < 3)
    fraction += '0';
  const qint64 fractional_ms = fraction.isEmpty() ? 0 : fraction.toLongLong();
  qint64 value = 0;
  if (hours > (kMaximumMilliseconds - value) / 3600000)
    return fail(error, "Highlight time exceeds the pipeline time range");
  value += hours * 3600000;
  if (minutes > (kMaximumMilliseconds - value) / 60000)
    return fail(error, "Highlight time exceeds the pipeline time range");
  value += minutes * 60000;
  if (seconds > (kMaximumMilliseconds - value) / 1000)
    return fail(error, "Highlight time exceeds the pipeline time range");
  value += seconds * 1000;
  if (fractional_ms > kMaximumMilliseconds - value)
    return fail(error, "Highlight time exceeds the pipeline time range");
  *milliseconds = value + fractional_ms;
  return true;
}

QString FormatHighlightTime(qint64 milliseconds) {
  if (!valid_milliseconds(milliseconds))
    return {};
  const qint64 hours = milliseconds / 3600000;
  const qint64 minutes = milliseconds / 60000 % 60;
  const qint64 seconds = milliseconds / 1000 % 60;
  const qint64 fraction = milliseconds % 1000;
  QString formatted = QString("%1:%2:%3")
                          .arg(hours, 2, 10, QLatin1Char('0'))
                          .arg(minutes, 2, 10, QLatin1Char('0'))
                          .arg(seconds, 2, 10, QLatin1Char('0'));
  if (fraction != 0)
    formatted += QString(".%1").arg(fraction, 3, 10, QLatin1Char('0'));
  return formatted;
}

qint64 HighlightItemDuration(const HighlightInterval& item) {
  return item.is_card ? item.card.duration_ms : item.end_ms - item.start_ms;
}

bool ValidateHighlightAnnotation(const HighlightAnnotation& a, QString* error) {
  if (a.kind != "arrow" && a.kind != "box" && a.kind != "text")
    return fail(error, "Unknown annotation type");
  if (a.motion != "fixed" && a.motion != "keyframes" && a.motion != "track")
    return fail(error, "Unknown annotation movement");
  if (!valid_milliseconds(a.start_ms) || !valid_milliseconds(a.end_ms) || a.end_ms <= a.start_ms || a.blink_ms < 40 ||
      a.blink_ms > 5000)
    return fail(error, "Invalid annotation timing");
  for (double v : {a.x, a.y, a.dx, a.dy})
    if (!std::isfinite(v) || std::abs(v) > 2)
      return fail(error, "Annotation position must be between -2 and 2 image dimensions");
  if (!std::isfinite(a.size) || a.size < 0.005 || a.size > 0.4 || !std::isfinite(a.thickness) || a.thickness < 0.001 ||
      a.thickness > 0.1 || a.weight < 100 || a.weight > 900 || !valid_color(a.color) || a.text.size() > 256)
    return fail(error, "Invalid annotation size, weight, thickness, color or text");
  if (a.motion == "track" &&
      (a.database.isEmpty() || a.run_id.isEmpty() || a.track_id.isEmpty() || a.archive_path.isEmpty()))
    return fail(error, "Tracked cues need an explicit database, run, track and archive binding");
  if (a.motion != "fixed" && a.positions.isEmpty())
    return fail(error, "Moving cues need saved positions");
  qint64 previous = -1;
  if (a.positions.size() > 4000)
    return fail(error, "Too many annotation positions");
  for (const auto& p : a.positions) {
    if (!valid_milliseconds(p.time_ms) || p.time_ms <= previous || !std::isfinite(p.x) || !std::isfinite(p.y) ||
        std::abs(p.x) > 2 || std::abs(p.y) > 2)
      return fail(error, "Positions need increasing times and finite image coordinates");
    previous = p.time_ms;
  }
  return true;
}

bool NormalizeHighlightInterval(HighlightInterval* interval, QString* error) {
  if (!interval)
    return fail(error, "No highlight interval was supplied");
  if (interval->is_card) {
    const auto& c = interval->card;
    if (c.duration_ms <= 0 || c.duration_ms > 600000 || !valid_color(c.color) || !valid_color(c.background) ||
        !std::isfinite(c.size) || c.size < 0.02 || c.size > 0.2 || c.weight < 100 || c.weight > 900 ||
        c.heading.size() > 256 || c.team_a.size() > 128 || c.team_b.size() > 128)
      return fail(error, "Cards need a duration up to ten minutes and valid styling/text");
    if (c.matchup &&
        (c.team_a.trimmed().isEmpty() || c.team_b.trimmed().isEmpty() ||
         !QDate::fromString(c.date, "yyyy-MM-dd").isValid() ||
         QDate::fromString(c.date, "yyyy-MM-dd").toString("yyyy-MM-dd") != c.date))
      return fail(error, "Matchup cards require both team names and a valid YYYY-MM-DD game date");
    return true;
  }
  if (interval->annotations.size() > 64)
    return fail(error, "A clip supports at most 64 annotations");
  for (const auto& a : interval->annotations)
    if (!ValidateHighlightAnnotation(a, error))
      return false;
  if (interval->event_mode) {
    if (!valid_milliseconds(interval->event_ms) || !valid_milliseconds(interval->duration_ms) ||
        interval->duration_ms == 0) {
      return fail(error, "Event time must be nonnegative and duration must be positive within the pipeline time range");
    }
    const qint64 before_ms = interval->duration_ms / 4 * 3 + interval->duration_ms % 4 * 3 / 4;
    const qint64 after_ms = interval->duration_ms - before_ms;
    if (interval->event_ms > kMaximumMilliseconds - after_ms)
      return fail(error, "Event interval exceeds the pipeline time range");
    interval->start_ms = std::max<qint64>(0, interval->event_ms - before_ms);
    interval->end_ms = interval->event_ms + after_ms;
  } else if (
      !valid_milliseconds(interval->start_ms) || !valid_milliseconds(interval->end_ms) ||
      interval->end_ms <= interval->start_ms) {
    return fail(error, "Interval end must be later than its nonnegative start within the pipeline time range");
  }
  return true;
}

bool LoadHighlightPlan(const QString& path, HighlightPlan* plan, QString* error) {
  if (!plan)
    return fail(error, "No highlight plan destination was supplied");
  const QFileInfo info(path);
  if (!info.exists() && !info.isSymLink()) {
    *plan = HighlightPlan{};
    return true;
  }
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) {
    return fail(error, QString("Could not read highlights at %1: %2").arg(path, file.errorString()));
  }
  if (file.size() > 32 * 1024 * 1024)
    return fail(error, "Highlight plan exceeds 32 MiB");
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, QString("Invalid highlights JSON: %1").arg(parse_error.errorString()));
  const QJsonObject root = document.object();
  const int schema = root.value("schema").toInt(-1);
  if (schema != 1 && schema != 2)
    return fail(error, "Unsupported highlights schema");
  if (!root.value("base_name").isString() || !valid_base_name(root.value("base_name").toString()))
    return fail(error, "Highlight base name must use letters, numbers, spaces, '_' or '-', up to 64 characters");
  if (!root.value("intervals").isArray())
    return fail(error, "Highlight intervals must be an array");

  HighlightPlan loaded;
  loaded.base_name = root.value("base_name").toString();
  if (schema == 2) {
    if (!root.value("archive_path").isString())
      return fail(error, "Reel archive path must be a string");
    loaded.archive_path = root.value("archive_path").toString();
  }
  const QJsonArray rows = root.value("intervals").toArray();
  if (rows.size() > 1000)
    return fail(error, "A reel supports at most 1000 items");
  loaded.intervals.reserve(rows.size());
  qint64 position_count = 0;
  for (qsizetype index = 0; index < rows.size(); ++index) {
    if (!rows.at(index).isObject())
      return fail(error, QString("Highlight %1 must be an object").arg(index + 1));
    const QJsonObject row = rows.at(index).toObject();
    if (!row.value("label").isString() || !row.value("event_mode").isBool())
      return fail(error, QString("Highlight %1 has an invalid label or mode").arg(index + 1));
    HighlightInterval interval;
    if (schema == 2) {
      if (!row.value("is_card").isBool() || !row.value("annotations").isArray() || !row.value("card").isObject())
        return fail(error, "Invalid reel item");
      interval.is_card = row.value("is_card").toBool();
      if (!read_card(row.value("card").toObject(), &interval.card, error))
        return false;
      for (const auto& v : row.value("annotations").toArray()) {
        HighlightAnnotation a;
        if (!v.isObject() || !read_annotation(v.toObject(), &a, error))
          return false;
        position_count += a.positions.size();
        if (position_count > 250000)
          return fail(error, "Reel exceeds 250,000 retained positions");
        interval.annotations.append(a);
      }
    }
    interval.label = row.value("label").toString();
    interval.event_mode = row.value("event_mode").toBool();
    if (!read_milliseconds(row, "event_ms", &interval.event_ms, error) ||
        !read_milliseconds(row, "duration_ms", &interval.duration_ms, error) ||
        !read_milliseconds(row, "start_ms", &interval.start_ms, error) ||
        !read_milliseconds(row, "end_ms", &interval.end_ms, error) || !NormalizeHighlightInterval(&interval, error)) {
      if (error)
        *error = QString("Highlight %1: %2").arg(index + 1).arg(*error);
      return false;
    }
    loaded.intervals.push_back(std::move(interval));
  }
  *plan = std::move(loaded);
  return true;
}

bool SaveHighlightPlan(const QString& path, const HighlightPlan& plan, QString* error) {
  if (!valid_base_name(plan.base_name))
    return fail(error, "Highlight base name must use letters, numbers, spaces, '_' or '-', up to 64 characters");
  QJsonArray rows;
  qint64 position_count = 0;
  if (plan.intervals.size() > 1000)
    return fail(error, "A reel supports at most 1000 items");
  for (qsizetype index = 0; index < plan.intervals.size(); ++index) {
    HighlightInterval interval = plan.intervals.at(index);
    if (!NormalizeHighlightInterval(&interval, error)) {
      if (error)
        *error = QString("Highlight %1: %2").arg(index + 1).arg(*error);
      return false;
    }
    QJsonArray annotations;
    for (const auto& a : interval.annotations) {
      position_count += a.positions.size();
      if (position_count > 250000)
        return fail(error, "Reel exceeds 250,000 retained positions");
      annotations.append(annotation_json(a));
    }
    rows.append(
        QJsonObject{
            {"is_card", interval.is_card},
            {"card", card_json(interval.card)},
            {"annotations", annotations},
            {"label", interval.label},
            {"event_mode", interval.event_mode},
            {"event_ms", interval.event_ms},
            {"duration_ms", interval.duration_ms},
            {"start_ms", interval.start_ms},
            {"end_ms", interval.end_ms},
        });
  }
  const QJsonObject root{
      {"schema", 2}, {"base_name", plan.base_name}, {"archive_path", plan.archive_path}, {"intervals", rows}};
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return fail(error, QString("Could not save highlights at %1: %2").arg(path, file.errorString()));
  const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (contents.size() > 32 * 1024 * 1024)
    return fail(error, "Highlight plan exceeds 32 MiB");
  if (file.write(contents) != contents.size() || !file.commit())
    return fail(error, QString("Could not commit highlights at %1: %2").arg(path, file.errorString()));
  return true;
}

} // namespace hm::ui
