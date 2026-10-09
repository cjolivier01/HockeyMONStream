#include "src/apps/hstream-ui/TelemetryDbPublisher.h"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryFile>
#include <cerrno>
#include <stdexcept>
#include "hstream/src/libs/recording/Database.h"

namespace {
class PublicationLock {
 public:
  explicit PublicationLock(const QString& directory) {
    const QByteArray path = QFile::encodeName(QDir(directory).filePath(".hm-output-publication.lock"));
    fd_ = open(path.constData(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0)
      throw std::runtime_error("Cannot open telemetry publication lock");
    struct stat info {};
    if (fstat(fd_, &info) != 0 || !S_ISREG(info.st_mode) || flock(fd_, LOCK_EX) != 0) {
      close(fd_);
      throw std::runtime_error("Cannot acquire telemetry publication lock");
    }
  }
  ~PublicationLock() {
    close(fd_);
  }
  PublicationLock(const PublicationLock&) = delete;
  PublicationLock& operator=(const PublicationLock&) = delete;

 private:
  int fd_{-1};
};

QString resolved_existing_directory_path(const QString& directory) {
  const QFileInfo info(directory);
  const QString canonical = info.canonicalFilePath();
  return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}
} // namespace

namespace hm::ui_internal {
TelemetryCsvPublicationResult publish_telemetry_database(
    const QString& source,
    const QString& directory,
    const std::optional<QString>& suffix) {
  TelemetryCsvPublicationResult result;
  try {
    if (suffix && !QRegularExpression(R"(^-0*[1-9]\d*$)").match(*suffix).hasMatch())
      throw std::runtime_error("Invalid database generation suffix");
    std::optional<uint64_t> fixed_generation;
    if (suffix) {
      bool valid = true;
      fixed_generation = suffix->mid(1).toULongLong(&valid);
      if (!valid)
        throw std::runtime_error("Database generation exceeds integer range");
    }
    const QString publication_directory = resolved_existing_directory_path(directory);
    if (!QDir(publication_directory).exists())
      throw std::runtime_error("Game directory does not exist");
    const PublicationLock publication_lock(publication_directory);
    hm::recording::Database input(source.toStdString());
    input.Validate();
    input.Exec("BEGIN");
    hm::recording::Statement runs(input.get(), "SELECT count(*),sum(completed),sum(sample_count) FROM runs");
    if (!runs.Next() || !runs.Int(0) || runs.Int(0) != runs.Int(1) || runs.Int(2) <= 0)
      throw std::runtime_error("Only completed recordings can be published");
    hm::recording::Statement games(input.get(), "SELECT DISTINCT game_id FROM runs");
    if (!games.Next())
      throw std::runtime_error("Telemetry database has no source game ID");
    const std::string game_id = games.Text(0);
    if (games.Next())
      throw std::runtime_error("Game-directory publication requires recordings from one game");
    hm::recording::TelemetryDatabaseStem(game_id);
    if (suffix && !telemetry_csv_destination_paths_available(publication_directory, *suffix))
      throw std::runtime_error("Telemetry generation already exists or cannot be checked");
    QTemporaryFile stage(QDir(publication_directory).filePath(".hstream-database-XXXXXX"));
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
    const int dir =
        open(QFile::encodeName(publication_directory).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0)
      throw std::runtime_error("Cannot open database publication directory");
    QString destination;
    const qint64 first = suffix ? 1 : next_archive_generation(publication_directory);
    if (first < 1) {
      close(dir);
      throw std::runtime_error("Cannot determine next database generation");
    }
    for (uint64_t generation = first;; ++generation) {
      const uint64_t number = fixed_generation.value_or(generation);
      const QString filename = suffix
          ? QString::fromStdString(hm::recording::TelemetryDatabaseStem(game_id)) + *suffix + ".db"
          : QString::fromStdString(hm::recording::TelemetryDatabaseFilename(game_id, number));
      destination = QDir(publication_directory).filePath(filename);
      if (link(QFile::encodeName(staged_path).constData(), QFile::encodeName(destination).constData()) == 0)
        break;
      if (errno != EEXIST || suffix || generation == UINT64_MAX) {
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
