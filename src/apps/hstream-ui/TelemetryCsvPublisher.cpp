#include "src/apps/hstream-ui/TelemetryCsvPublisher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <vector>

namespace hm::ui_internal {
namespace {

struct CopiedArtifact {
  QString filename;
  int fd{-1};
  struct stat identity{};
  off_t size{0};
};

bool same_identity(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

QString errno_string(int value) {
  return QString::fromLocal8Bit(std::strerror(value));
}

bool path_has_identity(int directory_fd, const QByteArray& filename, const struct stat& expected) {
  struct stat actual{};
  return ::fstatat(directory_fd, filename.constData(), &actual, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(actual.st_mode) &&
      same_identity(actual, expected);
}

bool unlink_if_owned(int directory_fd, const QByteArray& filename, const struct stat& expected) {
  return path_has_identity(directory_fd, filename, expected) && ::unlinkat(directory_fd, filename.constData(), 0) == 0;
}

bool sync_fd(int fd) {
  while (::fsync(fd) != 0) {
    if (errno == EINTR)
      continue;
    return false;
  }
  return true;
}

std::optional<QByteArray> read_regular_file_no_follow(const QString& path, qsizetype maximum_size, QString* error) {
  const QByteArray encoded_path = QFile::encodeName(QFileInfo(path).absoluteFilePath());
  const int fd = ::open(encoded_path.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0) {
    if (error)
      *error = errno_string(errno);
    return std::nullopt;
  }
  struct stat identity{};
  if (::fstat(fd, &identity) != 0 || !S_ISREG(identity.st_mode) || identity.st_size < 0 ||
      identity.st_size > maximum_size) {
    if (error)
      *error = "file is not a bounded regular file";
    ::close(fd);
    return std::nullopt;
  }
  QByteArray contents;
  contents.resize(static_cast<qsizetype>(identity.st_size));
  qsizetype offset = 0;
  while (offset < contents.size()) {
    const ssize_t count = ::pread(fd, contents.data() + offset, contents.size() - offset, offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      if (error)
        *error = count == 0 ? "file became shorter while being read" : errno_string(errno);
      ::close(fd);
      return std::nullopt;
    }
    offset += count;
  }
  struct stat named_identity{};
  if (::lstat(encoded_path.constData(), &named_identity) != 0 || !same_identity(identity, named_identity)) {
    if (error)
      *error = "file pathname changed while being read";
    ::close(fd);
    return std::nullopt;
  }
  ::close(fd);
  return contents;
}

bool copy_regular_file_no_replace(
    const QString& source_path,
    int destination_directory_fd,
    const QString& destination_filename,
    CopiedArtifact* copied,
    QString* error) {
  if (!copied)
    return false;
  const QByteArray encoded_source = QFile::encodeName(QFileInfo(source_path).absoluteFilePath());
  const QByteArray encoded_destination = QFile::encodeName(destination_filename);
  const int source_fd = ::open(encoded_source.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (source_fd < 0) {
    if (error)
      *error = QString("could not open %1: %2").arg(source_path, errno_string(errno));
    return false;
  }
  struct stat source_identity{};
  struct stat named_source_identity{};
  if (::fstat(source_fd, &source_identity) != 0 || !S_ISREG(source_identity.st_mode) ||
      ::lstat(encoded_source.constData(), &named_source_identity) != 0 ||
      !same_identity(source_identity, named_source_identity)) {
    if (error)
      *error = QString("telemetry source is not a stable regular file: %1").arg(source_path);
    ::close(source_fd);
    return false;
  }

  const int destination_fd = ::openat(
      destination_directory_fd,
      encoded_destination.constData(),
      O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
      S_IRUSR | S_IWUSR);
  if (destination_fd < 0) {
    if (error)
      *error = QString("could not reserve %1: %2").arg(destination_filename, errno_string(errno));
    ::close(source_fd);
    return false;
  }
  struct stat destination_identity{};
  if (::fstat(destination_fd, &destination_identity) != 0 || !S_ISREG(destination_identity.st_mode)) {
    if (error)
      *error = QString("could not identify new telemetry copy %1").arg(destination_filename);
    ::close(source_fd);
    ::close(destination_fd);
    return false;
  }

  bool success = true;
  QString failure;
  off_t offset = 0;
  std::array<char, 256 * 1024> buffer{};
  while (success) {
    const ssize_t count = ::pread(source_fd, buffer.data(), buffer.size(), offset);
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      success = false;
      failure = QString("could not read %1: %2").arg(source_path, errno_string(errno));
      break;
    }
    ssize_t written = 0;
    while (written < count) {
      const ssize_t result = ::write(destination_fd, buffer.data() + written, count - written);
      if (result < 0 && errno == EINTR)
        continue;
      if (result <= 0) {
        success = false;
        failure = QString("could not write %1: %2").arg(destination_filename, errno_string(result == 0 ? EIO : errno));
        break;
      }
      written += result;
    }
    offset += count;
  }
  struct stat final_source_identity{};
  struct stat final_named_source_identity{};
  struct stat final_destination_identity{};
  struct stat final_named_destination_identity{};
  if (success && !sync_fd(destination_fd)) {
    success = false;
    failure = QString("could not sync %1: %2").arg(destination_filename, errno_string(errno));
  }
  if (success && ::fstat(source_fd, &final_source_identity) != 0) {
    success = false;
    failure = QString("could not revalidate %1: %2").arg(source_path, errno_string(errno));
  }
  if (success && ::lstat(encoded_source.constData(), &final_named_source_identity) != 0) {
    success = false;
    failure = QString("could not revalidate the path for %1: %2").arg(source_path, errno_string(errno));
  }
  if (success && ::fstat(destination_fd, &final_destination_identity) != 0) {
    success = false;
    failure = QString("could not revalidate %1: %2").arg(destination_filename, errno_string(errno));
  }
  if (success &&
      ::fstatat(
          destination_directory_fd,
          encoded_destination.constData(),
          &final_named_destination_identity,
          AT_SYMLINK_NOFOLLOW) != 0) {
    success = false;
    failure = QString("could not revalidate the path for %1: %2").arg(destination_filename, errno_string(errno));
  }
  if (success &&
      (!same_identity(source_identity, final_source_identity) ||
       !same_identity(source_identity, final_named_source_identity) || source_identity.st_size != offset ||
       final_source_identity.st_size != offset)) {
    success = false;
    failure = QString("telemetry source changed while being copied: %1").arg(source_path);
  }
  if (success &&
      (!S_ISREG(final_destination_identity.st_mode) || !S_ISREG(final_named_destination_identity.st_mode) ||
       !same_identity(destination_identity, final_destination_identity) ||
       !same_identity(destination_identity, final_named_destination_identity) ||
       final_destination_identity.st_size != offset || final_named_destination_identity.st_size != offset)) {
    success = false;
    failure = QString("telemetry destination changed or has the wrong size: %1").arg(destination_filename);
  }
  ::close(source_fd);
  if (!success) {
    if (!unlink_if_owned(destination_directory_fd, encoded_destination, destination_identity) &&
        path_has_identity(destination_directory_fd, encoded_destination, destination_identity)) {
      failure += QString("; could not remove incomplete destination: %1").arg(errno_string(errno));
    }
    if (error)
      *error = failure.isEmpty() ? QString("could not copy %1 to %2").arg(source_path, destination_filename) : failure;
    ::close(destination_fd);
    return false;
  }

  copied->filename = destination_filename;
  copied->fd = destination_fd;
  copied->identity = destination_identity;
  copied->size = offset;
  return true;
}

QString manifest_file(const QJsonObject& root, const QString& section, const QString& key) {
  return root.value(section).toObject().value(key).toObject().value("file").toString();
}

} // namespace

QString finalized_archive_csv_suffix(const QString& archive_path, const QString& game_id) {
  const QFileInfo archive(archive_path);
  if (archive.suffix().compare("mp4", Qt::CaseInsensitive) != 0)
    return {};
  QString safe_game_id = game_id.trimmed();
  safe_game_id.replace(QRegularExpression(R"([\\/]+)"), "_");
  const QString base = safe_game_id + "-tracking_output-with-audio";
  const QRegularExpression pattern(QString("^%1(?:-(\\d+))?$").arg(QRegularExpression::escape(base)));
  const QRegularExpressionMatch match = pattern.match(archive.completeBaseName());
  if (!match.hasMatch())
    return {};
  return match.captured(1).isEmpty() ? QString("") : "-" + match.captured(1);
}

TelemetryCsvPublicationResult publish_telemetry_csvs(
    const QString& manifest_path,
    const QString& game_directory,
    const QString& destination_suffix) {
  TelemetryCsvPublicationResult result;
  if (!QRegularExpression(R"(^(-\d+)?$)").match(destination_suffix).hasMatch()) {
    result.error = QString("invalid telemetry destination suffix: %1").arg(destination_suffix);
    return result;
  }

  QString manifest_error;
  const std::optional<QByteArray> manifest_contents =
      read_regular_file_no_follow(manifest_path, 1024 * 1024, &manifest_error);
  if (!manifest_contents.has_value()) {
    result.error = QString("could not read telemetry manifest %1: %2").arg(manifest_path, manifest_error);
    return result;
  }
  QJsonParseError parse_error{};
  const QJsonDocument document = QJsonDocument::fromJson(*manifest_contents, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    result.error = QString("telemetry manifest is invalid JSON: %1").arg(parse_error.errorString());
    return result;
  }
  const QJsonObject root = document.object();
  if (!root.value("completed").toBool() || root.value("publication_state").toString() != "committed") {
    result.error = "telemetry generation is not committed and complete";
    return result;
  }

  const QFileInfo manifest_info(manifest_path);
  const QRegularExpression manifest_pattern(R"(^hstream_telemetry(-\d+)?\.json$)");
  const QRegularExpressionMatch manifest_match = manifest_pattern.match(manifest_info.fileName());
  if (!manifest_match.hasMatch()) {
    result.error = "telemetry manifest filename does not identify a generation";
    return result;
  }
  const QString source_suffix = manifest_match.captured(1);
  const QJsonObject sidecars = root.value("sidecars").toObject();
  const std::array<std::pair<QString, QString>, 6> artifacts = {{
      {"camera", manifest_file(root, "hm_compatibility", "camera_csv")},
      {"camera_fast", manifest_file(root, "hm_compatibility", "camera_fast_csv")},
      {"detections", manifest_file(root, "hm_compatibility", "detections_csv")},
      {"hstream_frame_index", sidecars.value("frame_index").toString()},
      {"hstream_config_events", sidecars.value("config_events").toString()},
      {"tracking", manifest_file(root, "hm_compatibility", "tracking_csv")},
  }};
  for (const auto& [stem, filename] : artifacts) {
    if (filename != stem + source_suffix + ".csv" || QFileInfo(filename).fileName() != filename) {
      result.error = QString("telemetry manifest has an unsafe or inconsistent %1 filename").arg(stem);
      return result;
    }
  }

  const QByteArray encoded_game_directory = QFile::encodeName(QFileInfo(game_directory).absoluteFilePath());
  const int game_directory_fd =
      ::open(encoded_game_directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (game_directory_fd < 0) {
    result.error = QString("could not open game directory %1: %2").arg(game_directory, errno_string(errno));
    return result;
  }

  std::vector<CopiedArtifact> copied;
  copied.reserve(artifacts.size());
  auto rollback = [&]() -> QString {
    QString rollback_error;
    for (auto item = copied.rbegin(); item != copied.rend(); ++item) {
      CopiedArtifact& artifact = *item;
      unlink_if_owned(game_directory_fd, QFile::encodeName(artifact.filename), artifact.identity);
      if (path_has_identity(game_directory_fd, QFile::encodeName(artifact.filename), artifact.identity) &&
          rollback_error.isEmpty()) {
        rollback_error = QString("could not roll back %1: %2").arg(artifact.filename, errno_string(errno));
      }
      if (artifact.fd >= 0) {
        ::close(artifact.fd);
        artifact.fd = -1;
      }
    }
    if (!sync_fd(game_directory_fd) && rollback_error.isEmpty())
      rollback_error = QString("could not sync telemetry rollback: %1").arg(errno_string(errno));
    return rollback_error;
  };

  for (const auto& [stem, source_filename] : artifacts) {
    const QString source_path = manifest_info.dir().filePath(source_filename);
    const QString destination_filename = stem + destination_suffix + ".csv";
    CopiedArtifact artifact;
    QString copy_error;
    if (!copy_regular_file_no_replace(source_path, game_directory_fd, destination_filename, &artifact, &copy_error)) {
      result.error = copy_error;
      const QString rollback_error = rollback();
      if (!rollback_error.isEmpty())
        result.error += "; " + rollback_error;
      ::close(game_directory_fd);
      return result;
    }
    copied.push_back(std::move(artifact));
    // Make all companion directory entries durable before tracking appears.
    if (stem == "hstream_config_events" && !sync_fd(game_directory_fd)) {
      result.error = QString("could not sync telemetry companion files: %1").arg(errno_string(errno));
      const QString rollback_error = rollback();
      if (!rollback_error.isEmpty())
        result.error += "; " + rollback_error;
      ::close(game_directory_fd);
      return result;
    }
  }
  if (!sync_fd(game_directory_fd)) {
    result.error = QString("could not sync telemetry tracking commit: %1").arg(errno_string(errno));
    const QString rollback_error = rollback();
    if (!rollback_error.isEmpty())
      result.error += "; " + rollback_error;
    ::close(game_directory_fd);
    return result;
  }
  for (CopiedArtifact& artifact : copied) {
    struct stat final_identity{};
    const QByteArray filename = QFile::encodeName(artifact.filename);
    if (::fstatat(game_directory_fd, filename.constData(), &final_identity, AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISREG(final_identity.st_mode) || !same_identity(final_identity, artifact.identity) ||
        final_identity.st_size != artifact.size) {
      result.error = QString("published telemetry file changed before commit completed: %1").arg(artifact.filename);
      const QString rollback_error = rollback();
      if (!rollback_error.isEmpty())
        result.error += "; " + rollback_error;
      ::close(game_directory_fd);
      return result;
    }
  }
  for (CopiedArtifact& artifact : copied) {
    result.published_paths.push_back(QDir(game_directory).filePath(artifact.filename));
    ::close(artifact.fd);
    artifact.fd = -1;
  }
  ::close(game_directory_fd);
  result.ok = true;
  return result;
}

} // namespace hm::ui_internal
