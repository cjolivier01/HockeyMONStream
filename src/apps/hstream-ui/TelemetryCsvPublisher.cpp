#include "src/apps/hstream-ui/TelemetryCsvPublisher.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QRegularExpression>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

namespace hm::ui_internal {
namespace {

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}
  ~UniqueFd() {
    if (fd_ >= 0)
      ::close(fd_);
  }
  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;
  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      if (fd_ >= 0)
        ::close(fd_);
      fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
  }
  int get() const {
    return fd_;
  }

 private:
  int fd_{-1};
};

struct OpenRegularFile {
  UniqueFd fd;
  QByteArray filename;
  struct stat snapshot{};
};

struct CopiedArtifact {
  QString stem;
  QString destination_filename;
  QByteArray temporary_filename;
  OpenRegularFile source;
  UniqueFd destination_fd;
  struct stat destination_identity{};
  off_t size{0};
  bool linked{false};
};

enum class PathIdentityState { kOwned, kAbsent, kReplaced, kError };

struct PathIdentityResult {
  PathIdentityState state{PathIdentityState::kError};
  int error_number{0};
};

constexpr auto kNamedTemporarySuffix = ".hstream-publish-tmp";

bool same_identity(const struct stat& left, const struct stat& right) {
  return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}

bool same_timespec(const struct timespec& left, const struct timespec& right) {
  return left.tv_sec == right.tv_sec && left.tv_nsec == right.tv_nsec;
}

bool same_snapshot(const struct stat& left, const struct stat& right) {
  return same_identity(left, right) && left.st_size == right.st_size && same_timespec(left.st_mtim, right.st_mtim) &&
      same_timespec(left.st_ctim, right.st_ctim);
}

QString errno_string(int value) {
  return QString::fromLocal8Bit(std::strerror(value));
}

PathIdentityResult path_identity(
    int directory_fd,
    const QByteArray& filename,
    const struct stat& expected,
    const TelemetryCsvPublicationTestHooks* test_hooks = nullptr) {
  if (test_hooks && test_hooks->rollback_identity_error_filename == QFile::decodeName(filename))
    return {PathIdentityState::kError, EIO};
  struct stat actual{};
  if (::fstatat(directory_fd, filename.constData(), &actual, AT_SYMLINK_NOFOLLOW) == 0) {
    return {
        S_ISREG(actual.st_mode) && same_identity(actual, expected) ? PathIdentityState::kOwned
                                                                   : PathIdentityState::kReplaced,
        0};
  }
  const int lookup_error = errno;
  return {lookup_error == ENOENT ? PathIdentityState::kAbsent : PathIdentityState::kError, lookup_error};
}

QString path_identity_error(const QString& action, const QString& filename, const PathIdentityResult& identity) {
  if (identity.state == PathIdentityState::kReplaced)
    return QString("could not %1 %2 because the path was replaced").arg(action, filename);
  if (identity.state == PathIdentityState::kError)
    return QString("could not inspect %1 while trying to %2 it: %3")
        .arg(filename, action, errno_string(identity.error_number));
  return {};
}

bool sync_fd(int fd) {
  while (::fsync(fd) != 0) {
    if (errno == EINTR)
      continue;
    return false;
  }
  return true;
}

bool remove_owned_path(
    int directory_fd,
    const QByteArray& filename,
    const struct stat& expected,
    QString* error,
    const TelemetryCsvPublicationTestHooks* test_hooks = nullptr) {
  const PathIdentityResult before = path_identity(directory_fd, filename, expected, test_hooks);
  if (before.state == PathIdentityState::kAbsent)
    return true;
  if (before.state != PathIdentityState::kOwned) {
    if (error)
      *error = path_identity_error("remove", QFile::decodeName(filename), before);
    return false;
  }
  if (::unlinkat(directory_fd, filename.constData(), 0) != 0) {
    if (error)
      *error = QString("could not remove %1: %2").arg(QFile::decodeName(filename), errno_string(errno));
    return false;
  }
  const PathIdentityResult after = path_identity(directory_fd, filename, expected);
  if (after.state == PathIdentityState::kAbsent)
    return true;
  if (error) {
    *error = after.state == PathIdentityState::kOwned
        ? QString("removed path is still present: %1").arg(QFile::decodeName(filename))
        : path_identity_error("confirm removal of", QFile::decodeName(filename), after);
  }
  return false;
}

bool unnamed_temporary_files_unsupported(int error_number) {
  return error_number == EOPNOTSUPP || error_number == EINVAL || error_number == EISDIR || error_number == ENOENT ||
      error_number == ENOSYS;
}

bool create_named_temporary_file(
    int directory_fd,
    const QString& destination_filename,
    UniqueFd* destination_fd,
    QByteArray* temporary_filename,
    struct stat* destination_identity,
    QString* error) {
  if (!destination_fd || !temporary_filename || !destination_identity)
    return false;
  const QByteArray name = QFile::encodeName(destination_filename + kNamedTemporarySuffix);
  for (int attempt = 0; attempt < 3; ++attempt) {
    UniqueFd created(
        ::openat(
            directory_fd, name.constData(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR));
    if (created.get() >= 0) {
      struct stat identity{};
      if (::fstat(created.get(), &identity) != 0 || !S_ISREG(identity.st_mode)) {
        const int stat_error = errno;
        ::unlinkat(directory_fd, name.constData(), 0);
        if (error) {
          *error = QString("could not identify named telemetry staging file %1: %2")
                       .arg(QFile::decodeName(name), errno_string(stat_error));
        }
        return false;
      }
      if (::flock(created.get(), LOCK_EX | LOCK_NB) != 0) {
        const int lock_error = errno;
        QString ignored;
        remove_owned_path(directory_fd, name, identity, &ignored);
        if (error) {
          *error = QString("could not lock named telemetry staging file %1: %2")
                       .arg(QFile::decodeName(name), errno_string(lock_error));
        }
        return false;
      }
      *destination_fd = std::move(created);
      *temporary_filename = name;
      *destination_identity = identity;
      return true;
    }
    const int create_error = errno;
    if (create_error != EEXIST) {
      if (error) {
        *error = QString("could not create named telemetry staging file %1: %2")
                     .arg(QFile::decodeName(name), errno_string(create_error));
      }
      return false;
    }

    UniqueFd stale(::openat(directory_fd, name.constData(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (stale.get() < 0) {
      if (errno == ENOENT)
        continue;
      if (error) {
        *error = QString("could not inspect existing telemetry staging file %1: %2")
                     .arg(QFile::decodeName(name), errno_string(errno));
      }
      return false;
    }
    struct stat stale_identity{};
    if (::fstat(stale.get(), &stale_identity) != 0 || !S_ISREG(stale_identity.st_mode)) {
      if (error)
        *error = QString("existing telemetry staging path is not a regular file: %1").arg(QFile::decodeName(name));
      return false;
    }
    if (::flock(stale.get(), LOCK_EX | LOCK_NB) != 0) {
      if (error) {
        *error = errno == EWOULDBLOCK
            ? QString("another telemetry publication owns staging file %1").arg(QFile::decodeName(name))
            : QString("could not lock existing telemetry staging file %1: %2")
                  .arg(QFile::decodeName(name), errno_string(errno));
      }
      return false;
    }
    QString cleanup_error;
    if (!remove_owned_path(directory_fd, name, stale_identity, &cleanup_error)) {
      if (error)
        *error =
            QString("could not clean stale telemetry staging file %1: %2").arg(QFile::decodeName(name), cleanup_error);
      return false;
    }
    if (!sync_fd(directory_fd)) {
      if (error) {
        *error = QString("could not sync stale telemetry staging cleanup for %1: %2")
                     .arg(QFile::decodeName(name), errno_string(errno));
      }
      return false;
    }
  }
  if (error)
    *error = QString("could not exclusively create telemetry staging file %1").arg(QFile::decodeName(name));
  return false;
}

bool open_regular_file_at(int directory_fd, const QByteArray& filename, OpenRegularFile* opened, QString* error) {
  if (!opened)
    return false;
  UniqueFd fd(::openat(directory_fd, filename.constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
  if (fd.get() < 0) {
    if (error)
      *error = errno_string(errno);
    return false;
  }
  struct stat descriptor_info{};
  struct stat named_info{};
  if (::fstat(fd.get(), &descriptor_info) != 0 || !S_ISREG(descriptor_info.st_mode) ||
      ::fstatat(directory_fd, filename.constData(), &named_info, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(named_info.st_mode) || !same_snapshot(descriptor_info, named_info)) {
    if (error)
      *error = "file is not a stable regular file";
    return false;
  }
  opened->fd = std::move(fd);
  opened->filename = filename;
  opened->snapshot = descriptor_info;
  return true;
}

bool revalidate_open_file(int directory_fd, const OpenRegularFile& opened) {
  struct stat descriptor_info{};
  struct stat named_info{};
  return opened.fd.get() >= 0 && ::fstat(opened.fd.get(), &descriptor_info) == 0 &&
      ::fstatat(directory_fd, opened.filename.constData(), &named_info, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISREG(descriptor_info.st_mode) && S_ISREG(named_info.st_mode) &&
      same_snapshot(opened.snapshot, descriptor_info) && same_snapshot(opened.snapshot, named_info);
}

bool read_regular_file_at(
    int directory_fd,
    const QByteArray& filename,
    qsizetype maximum_size,
    QByteArray* contents,
    OpenRegularFile* opened,
    QString* error) {
  if (!contents || !open_regular_file_at(directory_fd, filename, opened, error))
    return false;
  if (opened->snapshot.st_size < 0 || opened->snapshot.st_size > maximum_size) {
    if (error)
      *error = "file is not a bounded regular file";
    return false;
  }
  QByteArray file_contents;
  file_contents.resize(static_cast<qsizetype>(opened->snapshot.st_size));
  qsizetype offset = 0;
  while (offset < file_contents.size()) {
    const ssize_t count =
        ::pread(opened->fd.get(), file_contents.data() + offset, file_contents.size() - offset, offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      if (error)
        *error = count == 0 ? "file became shorter while being read" : errno_string(errno);
      return false;
    }
    offset += count;
  }
  if (!revalidate_open_file(directory_fd, *opened)) {
    if (error)
      *error = "file changed while being read";
    return false;
  }
  *contents = std::move(file_contents);
  return true;
}

bool copy_regular_file_to_staging(
    int source_directory_fd,
    const QString& source_filename,
    int destination_directory_fd,
    const QString& stem,
    const QString& destination_filename,
    bool force_named_temporary_files,
    CopiedArtifact* copied,
    QString* error) {
  if (!copied)
    return false;
  OpenRegularFile source;
  if (!open_regular_file_at(source_directory_fd, QFile::encodeName(source_filename), &source, error)) {
    if (error)
      *error = QString("could not open %1: %2").arg(source_filename, *error);
    return false;
  }
  UniqueFd destination_fd;
  QByteArray temporary_filename;
  struct stat destination_identity{};
  if (!force_named_temporary_files) {
    destination_fd =
        UniqueFd(::openat(destination_directory_fd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR));
    if (destination_fd.get() < 0 && !unnamed_temporary_files_unsupported(errno)) {
      if (error) {
        *error = QString("could not create an unnamed copy for %1: %2").arg(destination_filename, errno_string(errno));
      }
      return false;
    }
  }
  if (destination_fd.get() < 0 &&
      !create_named_temporary_file(
          destination_directory_fd,
          destination_filename,
          &destination_fd,
          &temporary_filename,
          &destination_identity,
          error)) {
    return false;
  }
  if (temporary_filename.isEmpty() &&
      (::fstat(destination_fd.get(), &destination_identity) != 0 || !S_ISREG(destination_identity.st_mode))) {
    if (error)
      *error = QString("could not identify new telemetry copy %1").arg(destination_filename);
    return false;
  }

  bool success = true;
  QString failure;
  off_t offset = 0;
  std::array<char, 256 * 1024> buffer{};
  while (success) {
    const ssize_t count = ::pread(source.fd.get(), buffer.data(), buffer.size(), offset);
    if (count == 0)
      break;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      success = false;
      failure = QString("could not read %1: %2").arg(source_filename, errno_string(errno));
      break;
    }
    ssize_t written = 0;
    while (written < count) {
      const ssize_t result = ::write(destination_fd.get(), buffer.data() + written, count - written);
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
  struct stat final_destination_identity{};
  if (success && !sync_fd(destination_fd.get())) {
    success = false;
    failure = QString("could not sync %1: %2").arg(destination_filename, errno_string(errno));
  }
  if (success && !revalidate_open_file(source_directory_fd, source)) {
    success = false;
    failure = QString("telemetry source changed while being copied: %1").arg(source_filename);
  }
  if (success && ::fstat(destination_fd.get(), &final_destination_identity) != 0) {
    success = false;
    failure = QString("could not revalidate %1: %2").arg(destination_filename, errno_string(errno));
  }
  if (success &&
      (!S_ISREG(final_destination_identity.st_mode) ||
       !same_identity(destination_identity, final_destination_identity) ||
       final_destination_identity.st_size != offset)) {
    success = false;
    failure = QString("telemetry destination changed or has the wrong size: %1").arg(destination_filename);
  }
  if (!success) {
    if (!temporary_filename.isEmpty()) {
      QString cleanup_error;
      if (!remove_owned_path(destination_directory_fd, temporary_filename, destination_identity, &cleanup_error)) {
        failure += QString("; could not clean staging file: %1").arg(cleanup_error);
      } else if (!sync_fd(destination_directory_fd)) {
        failure += QString("; could not sync staging cleanup: %1").arg(errno_string(errno));
      }
    }
    if (error)
      *error =
          failure.isEmpty() ? QString("could not copy %1 to %2").arg(source_filename, destination_filename) : failure;
    return false;
  }

  copied->stem = stem;
  copied->destination_filename = destination_filename;
  copied->temporary_filename = temporary_filename;
  copied->source = std::move(source);
  copied->destination_fd = std::move(destination_fd);
  copied->destination_identity = destination_identity;
  copied->size = offset;
  return true;
}

bool link_staged_copy(int destination_directory_fd, CopiedArtifact* artifact, QString* error) {
  if (!artifact || artifact->destination_fd.get() < 0)
    return false;
  const QByteArray destination = QFile::encodeName(artifact->destination_filename);
  const bool named = !artifact->temporary_filename.isEmpty();
  const QByteArray source = named ? artifact->temporary_filename
                                  : QByteArray("/proc/self/fd/") + QByteArray::number(artifact->destination_fd.get());
  const int source_directory_fd = named ? destination_directory_fd : AT_FDCWD;
  const int link_flags = named ? 0 : AT_SYMLINK_FOLLOW;
  if (::linkat(
          source_directory_fd, source.constData(), destination_directory_fd, destination.constData(), link_flags) !=
      0) {
    if (error)
      *error = QString("could not publish %1: %2").arg(artifact->destination_filename, errno_string(errno));
    return false;
  }
  artifact->linked = true;
  struct stat named_info{};
  if (::fstatat(destination_directory_fd, destination.constData(), &named_info, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(named_info.st_mode) || !same_identity(named_info, artifact->destination_identity) ||
      named_info.st_size != artifact->size) {
    if (error)
      *error = QString("published telemetry path has the wrong identity: %1").arg(artifact->destination_filename);
    return false;
  }
  if (named) {
    QString cleanup_error;
    if (!remove_owned_path(
            destination_directory_fd, artifact->temporary_filename, artifact->destination_identity, &cleanup_error)) {
      if (error) {
        *error = QString("published %1 but could not clean its staging file: %2")
                     .arg(artifact->destination_filename, cleanup_error);
      }
      return false;
    }
    artifact->temporary_filename.clear();
  }
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

bool telemetry_csv_destination_paths_available(const QString& game_directory, const QString& destination_suffix) {
  if (!QRegularExpression(R"(^(-\d+)?$)").match(destination_suffix).hasMatch())
    return false;
  const QByteArray encoded_game_directory = QFile::encodeName(QFileInfo(game_directory).absoluteFilePath());
  UniqueFd game_directory_fd(
      ::open(encoded_game_directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (game_directory_fd.get() < 0)
    return false;
  const std::array<QString, 6> stems = {
      "camera", "camera_fast", "detections", "hstream_frame_index", "hstream_config_events", "tracking"};
  for (const QString& stem : stems) {
    struct stat info{};
    const QByteArray filename = QFile::encodeName(stem + destination_suffix + ".csv");
    if (::fstatat(game_directory_fd.get(), filename.constData(), &info, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
      return false;
  }
  return true;
}

TelemetryCsvPublicationResult publish_telemetry_csvs(
    const QString& manifest_path,
    const QString& game_directory,
    const QString& destination_suffix,
    const TelemetryCsvPublicationTestHooks* test_hooks) {
  TelemetryCsvPublicationResult result;
  if (!QRegularExpression(R"(^(-\d+)?$)").match(destination_suffix).hasMatch()) {
    result.error = QString("invalid telemetry destination suffix: %1").arg(destination_suffix);
    return result;
  }

  const QFileInfo manifest_info(manifest_path);
  const QRegularExpression manifest_pattern(R"(^hstream_telemetry(-\d+)?\.json$)");
  const QRegularExpressionMatch manifest_match = manifest_pattern.match(manifest_info.fileName());
  if (!manifest_match.hasMatch()) {
    result.error = "telemetry manifest filename does not identify a generation";
    return result;
  }
  const QByteArray encoded_source_directory = QFile::encodeName(manifest_info.dir().absolutePath());
  UniqueFd source_directory_fd(
      ::open(encoded_source_directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (source_directory_fd.get() < 0) {
    result.error = QString("could not open telemetry working directory %1: %2")
                       .arg(manifest_info.dir().absolutePath(), errno_string(errno));
    return result;
  }
  OpenRegularFile open_manifest;
  QByteArray manifest_contents;
  QString manifest_error;
  if (!read_regular_file_at(
          source_directory_fd.get(),
          QFile::encodeName(manifest_info.fileName()),
          1024 * 1024,
          &manifest_contents,
          &open_manifest,
          &manifest_error)) {
    result.error = QString("could not read telemetry manifest %1: %2").arg(manifest_path, manifest_error);
    return result;
  }
  QJsonParseError parse_error{};
  const QJsonDocument document = QJsonDocument::fromJson(manifest_contents, &parse_error);
  if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
    result.error = QString("telemetry manifest is invalid JSON: %1").arg(parse_error.errorString());
    return result;
  }
  const QJsonObject root = document.object();
  if (!root.value("completed").toBool() || root.value("publication_state").toString() != "committed") {
    result.error = "telemetry generation is not committed and complete";
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
  UniqueFd game_directory_fd(
      ::open(encoded_game_directory.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
  if (game_directory_fd.get() < 0) {
    result.error = QString("could not open game directory %1: %2").arg(game_directory, errno_string(errno));
    return result;
  }

  std::vector<CopiedArtifact> copied;
  copied.reserve(artifacts.size());
  auto cleanup_staging = [&]() -> QString {
    QString cleanup_error;
    bool removed_any = false;
    for (CopiedArtifact& artifact : copied) {
      if (artifact.temporary_filename.isEmpty())
        continue;
      QString item_error;
      if (!remove_owned_path(
              game_directory_fd.get(), artifact.temporary_filename, artifact.destination_identity, &item_error)) {
        if (cleanup_error.isEmpty())
          cleanup_error = item_error;
        continue;
      }
      artifact.temporary_filename.clear();
      removed_any = true;
    }
    if (removed_any && !sync_fd(game_directory_fd.get()) && cleanup_error.isEmpty()) {
      cleanup_error = QString("could not sync telemetry staging cleanup: %1").arg(errno_string(errno));
    }
    return cleanup_error;
  };
  auto append_error = [](QString* destination, const QString& addition) {
    if (addition.isEmpty())
      return;
    if (!destination->isEmpty())
      *destination += "; ";
    *destination += addition;
  };
  auto rollback = [&]() -> QString {
    QString rollback_error;
    auto tracking = std::find_if(
        copied.begin(), copied.end(), [](const CopiedArtifact& artifact) { return artifact.stem == "tracking"; });
    if (tracking != copied.end() && tracking->linked) {
      const QByteArray filename = QFile::encodeName(tracking->destination_filename);
      const PathIdentityResult tracking_identity =
          path_identity(game_directory_fd.get(), filename, tracking->destination_identity, test_hooks);
      if (tracking_identity.state == PathIdentityState::kOwned) {
        QString removal_error;
        if (!remove_owned_path(game_directory_fd.get(), filename, tracking->destination_identity, &removal_error)) {
          append_error(&rollback_error, removal_error);
          append_error(&rollback_error, cleanup_staging());
          return rollback_error + "; retaining complete telemetry set because tracking removal is ambiguous";
        }
      } else if (tracking_identity.state != PathIdentityState::kAbsent) {
        append_error(
            &rollback_error, path_identity_error("retract", tracking->destination_filename, tracking_identity));
        append_error(&rollback_error, cleanup_staging());
        return rollback_error + "; retaining complete telemetry set because tracking presence is ambiguous";
      }
      tracking->linked = false;
      if (!sync_fd(game_directory_fd.get())) {
        rollback_error =
            QString("could not durably retract %1: %2").arg(tracking->destination_filename, errno_string(errno));
        append_error(&rollback_error, cleanup_staging());
        return rollback_error + "; retaining telemetry companions because tracking removal is not durable";
      }
    }
    for (auto item = copied.rbegin(); item != copied.rend(); ++item) {
      if (!item->linked)
        continue;
      const QByteArray filename = QFile::encodeName(item->destination_filename);
      QString removal_error;
      if (remove_owned_path(game_directory_fd.get(), filename, item->destination_identity, &removal_error)) {
        item->linked = false;
      } else {
        append_error(&rollback_error, removal_error);
      }
    }
    if (!sync_fd(game_directory_fd.get())) {
      append_error(
          &rollback_error, QString("could not sync telemetry companion rollback: %1").arg(errno_string(errno)));
    }
    append_error(&rollback_error, cleanup_staging());
    return rollback_error;
  };

  for (const auto& [stem, source_filename] : artifacts) {
    const QString destination_filename = stem + destination_suffix + ".csv";
    CopiedArtifact artifact;
    QString copy_error;
    if (!copy_regular_file_to_staging(
            source_directory_fd.get(),
            source_filename,
            game_directory_fd.get(),
            stem,
            destination_filename,
            test_hooks && test_hooks->force_named_temporary_files,
            &artifact,
            &copy_error)) {
      result.error = copy_error;
      append_error(&result.error, cleanup_staging());
      return result;
    }
    copied.push_back(std::move(artifact));
  }
  auto sources_are_stable = [&]() {
    if (!revalidate_open_file(source_directory_fd.get(), open_manifest))
      return false;
    return std::all_of(copied.cbegin(), copied.cend(), [&](const CopiedArtifact& artifact) {
      return revalidate_open_file(source_directory_fd.get(), artifact.source);
    });
  };
  if (!sources_are_stable()) {
    result.error = "telemetry working files changed before publication";
    append_error(&result.error, cleanup_staging());
    return result;
  }

  for (CopiedArtifact& artifact : copied) {
    if (artifact.stem == "tracking")
      continue;
    QString link_error;
    if (!link_staged_copy(game_directory_fd.get(), &artifact, &link_error)) {
      result.error = link_error;
      const QString rollback_error = rollback();
      if (!rollback_error.isEmpty())
        result.error += "; " + rollback_error;
      return result;
    }
  }
  if (!sync_fd(game_directory_fd.get())) {
    result.error = QString("could not sync telemetry companion files: %1").arg(errno_string(errno));
    const QString rollback_error = rollback();
    if (!rollback_error.isEmpty())
      result.error += "; " + rollback_error;
    return result;
  }
  if (!sources_are_stable()) {
    result.error = "telemetry working files changed before the tracking commit";
    const QString rollback_error = rollback();
    if (!rollback_error.isEmpty())
      result.error += "; " + rollback_error;
    return result;
  }
  auto tracking = std::find_if(
      copied.begin(), copied.end(), [](const CopiedArtifact& artifact) { return artifact.stem == "tracking"; });
  QString tracking_error;
  if (tracking == copied.end() || !link_staged_copy(game_directory_fd.get(), &*tracking, &tracking_error)) {
    result.error = tracking_error.isEmpty() ? "tracking telemetry copy is unavailable" : tracking_error;
    const QString rollback_error = rollback();
    if (!rollback_error.isEmpty())
      result.error += "; " + rollback_error;
    return result;
  }
  const bool tracking_commit_synced =
      test_hooks && test_hooks->fail_tracking_commit_sync ? (errno = EIO, false) : sync_fd(game_directory_fd.get());
  if (!tracking_commit_synced) {
    result.error = QString("could not sync telemetry tracking commit: %1").arg(errno_string(errno));
    const QString rollback_error = rollback();
    if (!rollback_error.isEmpty())
      result.error += "; " + rollback_error;
    return result;
  }
  for (const CopiedArtifact& artifact : copied)
    result.published_paths.push_back(QDir(game_directory).filePath(artifact.destination_filename));
  result.ok = true;
  return result;
}

} // namespace hm::ui_internal
