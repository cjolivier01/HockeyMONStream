#include "src/apps/hstream-ui/WebScoreboardPreview.h"

#include "hstream/src/libs/common/PinnedFile.h"
#include "src/apps/hstream-ui/CalibrationImage.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QTemporaryFile>
#include <QtGui/QImageReader>
#include <stdexcept>

namespace hm::web {
namespace {
constexpr qint64 kMaximumSourceBytes = 2LL * 1024 * 1024 * 1024;
constexpr qint64 kMaximumPreviewBytes = 128LL * 1024 * 1024;

void require(bool ok, const char* message) {
  if (!ok)
    throw std::runtime_error(message);
}

struct Descriptor {
  explicit Descriptor(int value) : fd(value) {
    require(fd >= 0, "Cannot open scoreboard preview path");
  }
  ~Descriptor() {
    ::close(fd);
  }
  Descriptor(const Descriptor&) = delete;
  Descriptor& operator=(const Descriptor&) = delete;
  int fd;
};

QString descriptor_path(int fd) {
  return QString("/proc/self/fd/%1").arg(fd);
}

QString text(const QJsonObject& object, const char* key) {
  const auto value = object.value(key);
  require(value.isString() && !value.toString().isEmpty(), "Missing scoreboard preview field");
  require(!value.toString().contains(QChar(0)), "Invalid scoreboard preview field");
  return value.toString();
}

// Walk every component under a pinned descriptor. Neither ancestor symlinks nor
// relative traversal can change which directory owns the source or publication.
int open_path(int parent, const QString& path, bool directory) {
  const auto parts = path.split('/');
  require(!parts.isEmpty(), "Invalid scoreboard preview path");
  Descriptor current(::dup(parent));
  for (int index = 0; index < parts.size(); ++index) {
    const auto& part = parts[index];
    require(!part.isEmpty() && part != "." && part != "..", "Invalid scoreboard preview path");
    const bool final = index + 1 == parts.size();
    const int next = ::openat(
        current.fd,
        QFile::encodeName(part).constData(),
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK | ((!final || directory) ? O_DIRECTORY : 0));
    require(next >= 0, "Cannot open scoreboard preview path");
    ::close(current.fd);
    current.fd = next;
  }
  return ::dup(current.fd);
}

QJsonObject identity(int fd, bool regular = true) {
  struct stat status{};
  require(::fstat(fd, &status) == 0, "Cannot inspect scoreboard preview file");
  require(regular ? S_ISREG(status.st_mode) : S_ISDIR(status.st_mode), "Invalid scoreboard preview file type");
  return {
      {"device", QString::number(static_cast<qulonglong>(status.st_dev))},
      {"inode", QString::number(static_cast<qulonglong>(status.st_ino))},
      {"size", QString::number(status.st_size)},
      {"mtime_ns", QString::number(qint64(status.st_mtim.tv_sec) * 1000000000LL + status.st_mtim.tv_nsec)},
      {"ctime_ns", QString::number(qint64(status.st_ctim.tv_sec) * 1000000000LL + status.st_ctim.tv_nsec)},
  };
}

bool same_directory(int left, int right) {
  const auto a = identity(left, false), b = identity(right, false);
  return a["device"] == b["device"] && a["inode"] == b["inode"];
}

QString digest(QFile& file) {
  require(file.seek(0), "Cannot rewind scoreboard preview");
  QCryptographicHash hash(QCryptographicHash::Sha256);
  require(hash.addData(&file), "Cannot hash scoreboard preview");
  return QString::fromLatin1(hash.result().toHex());
}

void flush(QFile& file) {
  require(file.flush() && ::fsync(file.handle()) == 0, "Cannot sync scoreboard preview");
}
} // namespace

QJsonObject ScoreboardPreview(const QJsonObject& request, const std::function<void()>& before_publication) {
  require(request.value("schema") == "hstream-scoreboard-preview-request-v1", "Unsupported scoreboard preview schema");
  const auto token = text(request, "token");
  require(QRegularExpression("^[0-9a-f]{32}$").match(token).hasMatch(), "Invalid scoreboard preview token");
  const auto root_path = text(request, "root");
  require(root_path.startsWith('/') && root_path.size() > 1, "Scoreboard preview root must be absolute");
  const auto source_path = text(request, "source");
  require(source_path == "s.png" || source_path.endsWith("/s.png"), "Scoreboard preview requires the current s.png");
  Descriptor filesystem_root(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
  Descriptor root(open_path(filesystem_root.fd, root_path.mid(1), true));
  Descriptor source(open_path(root.fd, source_path, false));
  const auto source_identity = identity(source.fd);
  require(source_identity == request.value("source_identity").toObject(), "Scoreboard preview source changed");
  const auto source_bytes = source_identity.value("size").toString().toLongLong();
  require(source_bytes > 0 && source_bytes <= kMaximumSourceBytes, "Scoreboard preview source exceeds its size bound");

  const auto pinned_source = descriptor_path(source.fd);
  QImageReader reader(pinned_source, "png");
  const auto source_size = reader.size();
  const auto preview_size = scoreboardPreviewSize(source_size);
  require(!preview_size.isEmpty(), "Invalid scoreboard preview dimensions");
  // Match the desktop selection exactly: small PNGs retain Qt's decoder
  // (including interlacing); reduced PNGs use the shared bounded row decoder.
  const auto preview = preview_size == source_size ? reader.read() : readCalibrationPng(pinned_source, preview_size);
  require(!preview.isNull() && preview.size() == preview_size, "Cannot decode bounded scoreboard preview");

  const auto base = QString("scoreboard-preview-%1").arg(token);
  const auto image_name = (base + ".png").toLatin1();
  const auto receipt_name = (base + ".json").toLatin1();
  QTemporaryFile image(descriptor_path(root.fd) + "/." + base + "-image-XXXXXX");
  require(image.open() && preview.save(&image, "PNG"), "Cannot encode scoreboard preview");
  require(image.size() > 0 && image.size() <= kMaximumPreviewBytes, "Scoreboard preview exceeds its size bound");
  flush(image);
  const auto sha256 = digest(image);
  if (before_publication)
    before_publication();
  const auto check_source = [&] {
    Descriptor current_root(open_path(filesystem_root.fd, root_path.mid(1), true));
    require(same_directory(root.fd, current_root.fd), "Scoreboard preview run directory changed");
    Descriptor current_source(open_path(current_root.fd, source_path, false));
    require(
        identity(source.fd) == source_identity && identity(current_source.fd) == source_identity,
        "Scoreboard preview source changed");
  };
  check_source();
  bool image_published = false, receipt_published = false;
  try {
    require(
        hm::link_pinned_file(image.handle(), root.fd, image_name.constData()) == 0,
        "Scoreboard preview token already used or publication failed");
    image_published = true;
    require(
        ::unlink(QFile::encodeName(image.fileName()).constData()) == 0,
        "Cannot retire scoreboard preview temporary name");
    image.setAutoRemove(false);
    // Link/unlink changes ctime, so bind the final single-link identity.
    const QJsonObject result{
        {"schema", "hstream-scoreboard-preview-v1"},
        {"token", token},
        {"root", root_path},
        {"source", source_path},
        {"source_identity", source_identity},
        {"source_size", QJsonArray{source_size.width(), source_size.height()}},
        {"preview_size", QJsonArray{preview_size.width(), preview_size.height()}},
        {"image", QString::fromLatin1(image_name)},
        {"image_identity", identity(image.handle())},
        {"sha256", sha256}};
    QTemporaryFile receipt(descriptor_path(root.fd) + "/." + base + "-receipt-XXXXXX");
    const auto bytes = QJsonDocument(result).toJson(QJsonDocument::Compact);
    require(receipt.open() && receipt.write(bytes) == bytes.size(), "Cannot write scoreboard preview receipt");
    flush(receipt);
    check_source();
    require(
        hm::link_pinned_file(receipt.handle(), root.fd, receipt_name.constData()) == 0,
        "Scoreboard preview receipt already exists or publication failed");
    receipt_published = true;
    require(::fsync(root.fd) == 0, "Cannot sync scoreboard preview directory");
    return result;
  } catch (...) {
    if (receipt_published)
      ::unlinkat(root.fd, receipt_name.constData(), 0);
    if (image_published)
      ::unlinkat(root.fd, image_name.constData(), 0);
    throw;
  }
}
} // namespace hm::web
