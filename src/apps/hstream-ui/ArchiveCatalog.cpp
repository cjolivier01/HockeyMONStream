#include "src/apps/hstream-ui/ArchiveCatalog.h"

#include "src/apps/hstream-ui/HighlightPlan.h"

#include <algorithm>
#include <limits>

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <QtCore/QRegularExpression>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#endif

namespace hm::ui {
namespace {

constexpr const char* kSidecarSuffix = ".hstream-archive.json";

bool fail(QString* error, const QString& message) {
  if (error)
    *error = message;
  return false;
}

QString safe_game_id(const QString& game_id) {
  QString safe = game_id.trimmed();
  safe.replace(QRegularExpression(R"([\\/]+)"), "_");
  return safe;
}

// Rank used to order archives by fidelity: the full-resolution program output
// first, then its 4K reduction, then the raw stitched panorama.
int kind_rank(const QString& kind) {
  if (kind == "program")
    return 0;
  if (kind == "program_4k")
    return 1;
  return 2;
}

bool read_optional_int(const QJsonObject& object, const QString& key, int* value) {
  const QJsonValue json_value = object.value(key);
  if (!json_value.isDouble())
    return false;
  const double number = json_value.toDouble();
  if (number < 0 || number > std::numeric_limits<int>::max() ||
      number != static_cast<double>(static_cast<int>(number)))
    return false;
  *value = static_cast<int>(number);
  return true;
}

bool read_optional_milliseconds(const QJsonObject& object, const QString& key, qint64* value) {
  const QJsonValue json_value = object.value(key);
  if (!json_value.isDouble())
    return false;
  const double number = json_value.toDouble();
  if (number < 0 || number > 1e15 || number != static_cast<double>(static_cast<qint64>(number)))
    return false;
  *value = static_cast<qint64>(number);
  return true;
}

bool write_file_durably(const QString& path, const QByteArray& contents, QString* error) {
  const QString temporary_path = path + ".partial";
  {
    QFile file(temporary_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
      return fail(error, QString("Could not open %1: %2").arg(temporary_path, file.errorString()));
    const bool written = file.write(contents) == contents.size() && file.flush();
#ifdef Q_OS_UNIX
    const bool synced = written && ::fsync(file.handle()) == 0;
#else
    const bool synced = written;
#endif
    file.close();
    if (!synced) {
      QFile::remove(temporary_path);
      return fail(error, QString("Could not write %1").arg(temporary_path));
    }
  }
#ifdef Q_OS_UNIX
  if (::rename(QFile::encodeName(temporary_path).constData(), QFile::encodeName(path).constData()) != 0) {
    const QString reason = QString::fromLocal8Bit(std::strerror(errno));
    QFile::remove(temporary_path);
    return fail(error, QString("Could not replace %1: %2").arg(path, reason));
  }
  const int directory_fd =
      ::open(QFile::encodeName(QFileInfo(path).absolutePath()).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (directory_fd >= 0) {
    ::fsync(directory_fd);
    ::close(directory_fd);
  }
#else
  QFile::remove(path);
  if (!QFile::rename(temporary_path, path)) {
    QFile::remove(temporary_path);
    return fail(error, QString("Could not replace %1").arg(path));
  }
#endif
  return true;
}

} // namespace

QString ArchiveKindDisplayName(const QString& kind) {
  if (kind == "program")
    return "Program";
  if (kind == "program_4k")
    return "4K Program";
  if (kind == "stitched")
    return "Stitched";
  return kind;
}

QString ArchiveSidecarPath(const QString& archive_path) {
  return archive_path + QLatin1String(kSidecarSuffix);
}

QVector<ArchiveEntry> DiscoverArchives(const QString& game_dir, const QString& game_id) {
  const QString safe = safe_game_id(game_id);
  QVector<ArchiveEntry> entries;
  if (safe.isEmpty() || game_dir.isEmpty())
    return entries;
  const QRegularExpression pattern(
      QString(R"(^%1-(tracking|stitched|program_4k)_output(?:-with-audio)?(?:-(\d+))?(?i:\.mp4)$)")
          .arg(QRegularExpression::escape(safe)));
  QDir directory(game_dir);
  // No name filter: a game id is free text and QDir globs would read any
  // bracket or question mark in it as a wildcard, quietly matching nothing.
  // The pattern above is the only filter.
  const QStringList names = directory.entryList(QDir::Files | QDir::NoSymLinks, QDir::Name);
  for (const QString& name : names) {
    const QRegularExpressionMatch match = pattern.match(name);
    if (!match.hasMatch())
      continue;
    const QString path = directory.filePath(name);
    const QFileInfo info(path);
    if (!info.isFile() || info.size() <= 0)
      continue;
    const QFileInfo guard(path + ".hstream-pin");
    if (guard.exists() || guard.isSymLink())
      continue;
    ArchiveEntry entry;
    entry.path = path;
    entry.game_id = game_id;
    entry.kind = match.captured(1) == "tracking" ? QStringLiteral("program") : match.captured(1);
    entry.generation = match.captured(2).isEmpty() ? 0 : match.captured(2).toLongLong();
    LoadArchiveSidecar(path, &entry);
    entries.push_back(entry);
  }
  std::stable_sort(entries.begin(), entries.end(), [](const ArchiveEntry& left, const ArchiveEntry& right) {
    const int left_rank = kind_rank(left.kind);
    const int right_rank = kind_rank(right.kind);
    if (left_rank != right_rank)
      return left_rank < right_rank;
    return left.generation > right.generation;
  });
  return entries;
}

bool LoadArchiveSidecar(const QString& archive_path, ArchiveEntry* entry, QString* error) {
  if (!entry)
    return fail(error, "No archive entry destination was supplied");
  const QString path = ArchiveSidecarPath(archive_path);
  QFile file(path);
  if (!file.exists())
    return fail(error, QString("No archive sidecar at %1").arg(path));
  if (!file.open(QIODevice::ReadOnly))
    return fail(error, QString("Could not read %1: %2").arg(path, file.errorString()));
  QJsonParseError parse_error;
  const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject())
    return fail(error, QString("Invalid archive sidecar at %1: %2").arg(path, parse_error.errorString()));
  const QJsonObject root = document.object();
  if (root.value("schema").toInt(-1) != 1)
    return fail(error, QString("Unsupported archive sidecar schema at %1").arg(path));
  qint64 start_time_ms = 0;
  if (!read_optional_milliseconds(root, "start_time_ms", &start_time_ms))
    return fail(error, QString("Archive sidecar at %1 has no usable start time").arg(path));
  entry->start_time_ms = start_time_ms;
  entry->start_time_known = true;
  read_optional_milliseconds(root, "duration_ms", &entry->duration_ms);
  read_optional_int(root, "width", &entry->width);
  read_optional_int(root, "height", &entry->height);
  if (root.value("game_id").isString() && entry->game_id.isEmpty())
    entry->game_id = root.value("game_id").toString();
  if (root.value("kind").isString() && entry->kind.isEmpty())
    entry->kind = root.value("kind").toString();
  return true;
}

bool SaveArchiveSidecar(const ArchiveEntry& entry, QString* error) {
  if (entry.path.isEmpty())
    return fail(error, "No archive path was supplied");
  QJsonObject root{
      {"schema", 1},
      {"game_id", entry.game_id},
      {"kind", entry.kind},
      {"start_time", FormatHighlightTime(entry.start_time_ms)},
      {"start_time_ms", entry.start_time_ms},
  };
  if (entry.duration_ms >= 0)
    root.insert("duration_ms", entry.duration_ms);
  if (entry.width > 0 && entry.height > 0) {
    root.insert("width", entry.width);
    root.insert("height", entry.height);
  }
  return write_file_durably(
      ArchiveSidecarPath(entry.path), QJsonDocument(root).toJson(QJsonDocument::Indented), error);
}

} // namespace hm::ui
