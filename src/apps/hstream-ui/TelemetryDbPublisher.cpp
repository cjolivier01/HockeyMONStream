#include "src/apps/hstream-ui/TelemetryDbPublisher.h"
#include <fcntl.h>
#include <unistd.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryFile>
#include <cerrno>
#include <stdexcept>
#include "hstream/src/libs/recording/Database.h"

namespace hm::ui_internal {
TelemetryCsvPublicationResult publish_telemetry_database(
    const QString& source,
    const QString& directory,
    const std::optional<QString>& suffix) {
  TelemetryCsvPublicationResult result;
  try {
    if (suffix && !QRegularExpression(R"(^(-\d+)?$)").match(*suffix).hasMatch())
      throw std::runtime_error("Invalid database generation suffix");
    if (!QDir(directory).exists())
      throw std::runtime_error("Game directory does not exist");
    hm::recording::Database input(source.toStdString());
    input.Validate();
    input.Exec("BEGIN");
    hm::recording::Statement runs(input.get(), "SELECT count(*),sum(completed),sum(sample_count) FROM runs");
    if (!runs.Next() || !runs.Int(0) || runs.Int(0) != runs.Int(1) || runs.Int(2) <= 0)
      throw std::runtime_error("Only completed recordings can be published");
    QTemporaryFile stage(QDir(directory).filePath(".hstream-database-XXXXXX"));
    if (!stage.open())
      throw std::runtime_error(stage.errorString().toStdString());
    const QString staged_path = stage.fileName();
    stage.close();
    {
      hm::recording::Database output(staged_path.toStdString(), true);
      sqlite3_backup* backup = sqlite3_backup_init(output.get(), "main", input.get(), "main");
      if (!backup)
        throw std::runtime_error(sqlite3_errmsg(output.get()));
      int code;
      do {
        code = sqlite3_backup_step(backup, 256);
      } while (code == SQLITE_OK);
      const int finished = sqlite3_backup_finish(backup);
      if (code != SQLITE_DONE || finished != SQLITE_OK)
        throw std::runtime_error("Database snapshot copy failed");
      output.Validate();
      hm::recording::Statement check(output.get(), "PRAGMA quick_check");
      if (!check.Next() || check.Text(0) != "ok")
        throw std::runtime_error("Copied database failed integrity check");
    }
    const int file = open(QFile::encodeName(staged_path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (file < 0)
      throw std::runtime_error("Cannot synchronize database copy");
    const int synced = fsync(file);
    close(file);
    if (synced)
      throw std::runtime_error("Cannot synchronize database copy");
    const int dir = open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0)
      throw std::runtime_error("Cannot open database publication directory");
    QString destination;
    const qint64 first = suffix ? 0 : next_archive_generation(directory);
    if (first < 0) {
      close(dir);
      throw std::runtime_error("Cannot determine next database generation");
    }
    for (uint64_t generation = first;; ++generation) {
      const QString part = suffix ? *suffix : (generation ? "-" + QString::number(generation) : QString());
      destination = QDir(directory).filePath("hstream_telemetry" + part + ".db");
      if (link(QFile::encodeName(staged_path).constData(), QFile::encodeName(destination).constData()) == 0)
        break;
      if (errno != EEXIST || suffix) {
        close(dir);
        throw std::runtime_error("Database destination exists or cannot be published");
      }
    }
    const int committed = fsync(dir);
    close(dir);
    if (committed)
      throw std::runtime_error("Database copied but publication directory synchronization failed");
    result.ok = true;
    result.published_paths.append(destination);
  } catch (const std::exception& e) {
    result.error = QString::fromUtf8(e.what());
  }
  return result;
}
} // namespace hm::ui_internal
