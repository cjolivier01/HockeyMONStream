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
  OpenRegularFile source;
  UniqueFd destination_fd;
  struct stat destination_identity{};
  off_t size{0};
  bool linked{false};
};

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

bool copy_regular_file_to_unnamed(
    int source_directory_fd,
    const QString& source_filename,
    int destination_directory_fd,
    const QString& stem,
    const QString& destination_filename,
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
  UniqueFd destination_fd(::openat(destination_directory_fd, ".", O_TMPFILE | O_RDWR | O_CLOEXEC, S_IRUSR | S_IWUSR));
  if (destination_fd.get() < 0) {
    if (error)
      *error = QString("could not create an unnamed copy for %1: %2").arg(destination_filename, errno_string(errno));
    return false;
  }
  struct stat destination_identity{};
  if (::fstat(destination_fd.get(), &destination_identity) != 0 || !S_ISREG(destination_identity.st_mode)) {
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
    if (error)
      *error =
          failure.isEmpty() ? QString("could not copy %1 to %2").arg(source_filename, destination_filename) : failure;
    return false;
  }

  copied->stem = stem;
  copied->destination_filename = destination_filename;
  copied->source = std::move(source);
  copied->destination_fd = std::move(destination_fd);
  copied->destination_identity = destination_identity;
  copied->size = offset;
  return true;
}

bool link_unnamed_copy(int destination_directory_fd, CopiedArtifact* artifact, QString* error) {
  if (!artifact || artifact->destination_fd.get() < 0)
    return false;
  const QByteArray source = QByteArray("/proc/self/fd/") + QByteArray::number(artifact->destination_fd.get());
  const QByteArray destination = QFile::encodeName(artifact->destination_filename);
  if (::linkat(AT_FDCWD, source.constData(), destination_directory_fd, destination.constData(), AT_SYMLINK_FOLLOW) !=
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
    const QString& destination_suffix) {
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
  auto rollback = [&]() -> QString {
    QString rollback_error;
    auto tracking = std::find_if(
        copied.begin(), copied.end(), [](const CopiedArtifact& artifact) { return artifact.stem == "tracking"; });
    if (tracking != copied.end() && tracking->linked) {
      const QByteArray filename = QFile::encodeName(tracking->destination_filename);
      if (!unlink_if_owned(game_directory_fd.get(), filename, tracking->destination_identity) &&
          path_has_identity(game_directory_fd.get(), filename, tracking->destination_identity)) {
        return QString("could not retract %1: %2").arg(tracking->destination_filename, errno_string(errno));
      }
      tracking->linked = false;
      if (!sync_fd(game_directory_fd.get())) {
        return QString("could not durably retract %1: %2").arg(tracking->destination_filename, errno_string(errno));
      }
    }
    for (auto item = copied.rbegin(); item != copied.rend(); ++item) {
      if (!item->linked)
        continue;
      const QByteArray filename = QFile::encodeName(item->destination_filename);
      if (!unlink_if_owned(game_directory_fd.get(), filename, item->destination_identity) &&
          path_has_identity(game_directory_fd.get(), filename, item->destination_identity) &&
          rollback_error.isEmpty()) {
        rollback_error = QString("could not roll back %1: %2").arg(item->destination_filename, errno_string(errno));
      } else {
        item->linked = false;
      }
    }
    if (!sync_fd(game_directory_fd.get()) && rollback_error.isEmpty())
      rollback_error = QString("could not sync telemetry companion rollback: %1").arg(errno_string(errno));
    return rollback_error;
  };

  for (const auto& [stem, source_filename] : artifacts) {
    const QString destination_filename = stem + destination_suffix + ".csv";
    CopiedArtifact artifact;
    QString copy_error;
    if (!copy_regular_file_to_unnamed(
            source_directory_fd.get(),
            source_filename,
            game_directory_fd.get(),
            stem,
            destination_filename,
            &artifact,
            &copy_error)) {
      result.error = copy_error;
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
    return result;
  }

  for (CopiedArtifact& artifact : copied) {
    if (artifact.stem == "tracking")
      continue;
    QString link_error;
    if (!link_unnamed_copy(game_directory_fd.get(), &artifact, &link_error)) {
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
  if (tracking == copied.end() || !link_unnamed_copy(game_directory_fd.get(), &*tracking, &tracking_error)) {
    result.error = tracking_error.isEmpty() ? "tracking telemetry copy is unavailable" : tracking_error;
    const QString rollback_error = rollback();
    if (!rollback_error.isEmpty())
      result.error += "; " + rollback_error;
    return result;
  }
  if (!sync_fd(game_directory_fd.get())) {
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
