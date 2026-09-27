#include "src/apps/hstream-ui/HighlightPlan.h"

#include <algorithm>
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

bool NormalizeHighlightInterval(HighlightInterval* interval, QString* error) {
  if (!interval)
    return fail(error, "No highlight interval was supplied");
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
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, QString("Invalid highlights JSON: %1").arg(parse_error.errorString()));
  const QJsonObject root = document.object();
  if (root.value("schema").toInt(-1) != 1)
    return fail(error, "Unsupported highlights schema");
  if (!root.value("base_name").isString() || !valid_base_name(root.value("base_name").toString()))
    return fail(error, "Highlight base name must use letters, numbers, spaces, '_' or '-', up to 64 characters");
  if (!root.value("intervals").isArray())
    return fail(error, "Highlight intervals must be an array");

  HighlightPlan loaded;
  loaded.base_name = root.value("base_name").toString();
  const QJsonArray rows = root.value("intervals").toArray();
  loaded.intervals.reserve(rows.size());
  for (qsizetype index = 0; index < rows.size(); ++index) {
    if (!rows.at(index).isObject())
      return fail(error, QString("Highlight %1 must be an object").arg(index + 1));
    const QJsonObject row = rows.at(index).toObject();
    if (!row.value("label").isString() || !row.value("event_mode").isBool())
      return fail(error, QString("Highlight %1 has an invalid label or mode").arg(index + 1));
    HighlightInterval interval;
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
  for (qsizetype index = 0; index < plan.intervals.size(); ++index) {
    HighlightInterval interval = plan.intervals.at(index);
    if (!NormalizeHighlightInterval(&interval, error)) {
      if (error)
        *error = QString("Highlight %1: %2").arg(index + 1).arg(*error);
      return false;
    }
    rows.append(
        QJsonObject{
            {"label", interval.label},
            {"event_mode", interval.event_mode},
            {"event_ms", interval.event_ms},
            {"duration_ms", interval.duration_ms},
            {"start_ms", interval.start_ms},
            {"end_ms", interval.end_ms},
        });
  }
  const QJsonObject root{{"schema", 1}, {"base_name", plan.base_name}, {"intervals", rows}};
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly))
    return fail(error, QString("Could not save highlights at %1: %2").arg(path, file.errorString()));
  const QByteArray contents = QJsonDocument(root).toJson(QJsonDocument::Indented);
  if (file.write(contents) != contents.size() || !file.commit())
    return fail(error, QString("Could not commit highlights at %1: %2").arg(path, file.errorString()));
  return true;
}

} // namespace hm::ui
