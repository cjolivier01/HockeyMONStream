#include "src/apps/hstream-ui/WebJobSpec.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QSaveFile>
#include <QtCore/QStringList>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace hm::web {
namespace fs = std::filesystem;
namespace {
void require(bool condition, const std::string& message) {
  if (!condition)
    throw std::runtime_error(message);
}
void sync_directory(const fs::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(fd >= 0, "Cannot open publication directory");
  const int result = ::fsync(fd);
  ::close(fd);
  require(result == 0, "Cannot sync publication directory");
}
} // namespace
QString RequiredString(const QJsonObject& value, const QString& key, int maximum) {
  const auto field = value.value(key);
  require(field.isString(), "Expected string field " + key.toStdString());
  const QString text = field.toString();
  require(
      !text.isEmpty() && text.size() <= maximum && !text.contains(QChar(0)),
      "Invalid string field " + key.toStdString());
  return text;
}
double Number(const QJsonObject& value, const QString& key, double minimum, double maximum, double fallback) {
  if (!value.contains(key))
    return fallback;
  const auto field = value.value(key);
  const double number = field.toDouble();
  require(
      field.isDouble() && std::isfinite(number) && number >= minimum && number <= maximum,
      "Invalid numeric field " + key.toStdString());
  return number;
}
fs::path Contained(const fs::path& root, const QString& relative, bool must_exist) {
  require(!relative.isEmpty() && relative.size() <= 2048 && !relative.contains(QChar(0)), "Invalid relative path");
  const fs::path input(relative.toStdString());
  require(!input.is_absolute(), "Paths must be relative to the job root");
  for (const auto& component : input)
    require(component != "..", "Parent traversal is forbidden");
  const auto base = fs::canonical(root);
  const auto resolved = fs::weakly_canonical(base / input);
  const auto tail = resolved.lexically_relative(base);
  require(!tail.empty() && !tail.is_absolute() && *tail.begin() != "..", "Path escapes job root");
  require(!must_exist || fs::exists(resolved), "Referenced job input does not exist");
  return resolved;
}
QJsonObject ReadObject(const fs::path& path, std::size_t maximum) {
  QFile input(QString::fromStdString(path.string()));
  require(input.open(QIODevice::ReadOnly), "Cannot read JSON document");
  require(input.size() >= 0 && std::uint64_t(input.size()) <= maximum, "JSON document exceeds its size bound");
  const auto contents = input.read(maximum + 1);
  require(std::size_t(contents.size()) <= maximum, "JSON document exceeds its size bound");
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(contents, &error);
  require(error.error == QJsonParseError::NoError && document.isObject(), "Expected a valid JSON object");
  return document.object();
}
void WriteObject(const fs::path& path, const QJsonObject& value) {
  const auto contents = QJsonDocument(value).toJson(QJsonDocument::Compact);
  require(std::size_t(contents.size()) <= kMaximumResultBytes, "Result exceeds its size bound");
  QSaveFile output(QString::fromStdString(path.string()));
  require(output.open(QIODevice::WriteOnly), "Cannot create result document");
  require(
      output.write(contents) == contents.size() && output.flush() && ::fsync(output.handle()) == 0,
      "Cannot sync result document");
  require(output.commit(), "Cannot publish result document");
  sync_directory(path.parent_path());
}
QJsonObject Capabilities() {
  const QJsonArray operations{
      "calibration-complete",
      "calibration-view-read",
      "calibration-view-estimate",
      "calibration-view-landmarks",
      "calibration-view-preview",
      "calibration-view-save",
      "calibration-view-reset",
      "calibration-view-rotation-stage",
      "rink-map-read",
      "rink-map-update",
      "rink-map-save",
      "highlight-render",
      "highlight-preview",
      "highlight-live-preview",
      "highlight-card-preview",
      "highlight-archive-info",
      "matches-read",
      "matches-publish",
      "experiment-create",
      "experiment-edit",
      "experiment-prepare-edit",
      "experiment-save-matches",
      "experiment-recover",
      "experiment-run",
      "experiment-scan",
      "experiment-reuse-selection",
      "experiment-inspect",
      "experiment-complete",
      "experiment-promote",
      "track-runs",
      "track-choices",
      "track-capture",
      "camera-replay",
      "camera-experiment"};
  return {
      {"schema", 1},
      {"request_schema", 1},
      {"receipt_schema", 1},
      {"operations", operations},
      {"features",
       QJsonArray{
           "experiment-optics-filtering-v1",
           "match-editor-v1",
           "rink-editor-v1",
           "rink-map-v1",
           "highlight-frame-inspection-v1",
           "highlight-pinned-archive-v1",
           "highlight-archive-info-v1",
           "highlight-card-without-archive-v1",
           "camera-legacy-recording-v1",
           "camera-view-catalog-v3",
           "saved-calibration-view-v1",
           "saved-calibration-reset-v1",
           "calibration-view-rotation-stage-v1"}},
      {"maximum_request_bytes", int(kMaximumRequestBytes)},
      {"maximum_result_bytes", int(kMaximumResultBytes)}};
}
Request LoadRequest(const fs::path& filename) {
  const auto document = ReadObject(filename);
  require(document.value("schema").toInt() == 1, "Unsupported workflow request schema");
  Request request;
  request.root = fs::canonical(RequiredString(document, "root").toStdString());
  require(fs::canonical(filename).parent_path() == request.root, "Request must be stored directly inside its job root");
  request.game = Contained(request.root, RequiredString(document, "game_dir"));
  require(fs::is_directory(request.game), "Game directory is unavailable");
  request.run_id = RequiredString(document, "run_id", 64).toStdString();
  require(std::regex_match(request.run_id, std::regex("[A-Za-z0-9_-]{8,64}")), "Invalid run identity");
  request.operation = RequiredString(document, "operation", 64).toStdString();
  require(
      Capabilities().value("operations").toArray().contains(QString::fromStdString(request.operation)),
      "Unsupported workflow operation");
  require(document.value("payload").isObject(), "Expected an operation payload");
  request.payload = document.value("payload").toObject();
  request.runtime = document.value("runtime").toObject();
  require(
      !document.contains("authorized_media") || document.value("authorized_media").isArray(),
      "Invalid media authorization");
  request.authorized_media = document.value("authorized_media").toArray();
  return request;
}
std::string Relative(const Request& request, const fs::path& path) {
  return Contained(request.root, QString::fromStdString(path.lexically_relative(request.root).generic_string()))
      .lexically_relative(request.root)
      .generic_string();
}
QJsonObject Artifact(const Request& request, const fs::path& path, const QString& kind) {
  const auto relative = Relative(request, path);
  struct stat info{};
  require(
      ::lstat(path.c_str(), &info) == 0 && S_ISREG(info.st_mode) && info.st_size > 0,
      "Artifact must be a nonempty regular file");
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  require(fd >= 0, "Cannot open artifact for synchronization");
  const int result = ::fsync(fd);
  ::close(fd);
  require(result == 0, "Cannot synchronize artifact");
  // Identity integers are decimal strings: nanosecond timestamps exceed JSON's
  // exact double range. Consumers must compare integer values, never floats.
  return {
      {"path", QString::fromStdString(relative)},
      {"kind", kind},
      {"size", QString::number(info.st_size)},
      {"device", QString::number(info.st_dev)},
      {"inode", QString::number(info.st_ino)},
      {"mtime_ns", QString::number(qint64(info.st_mtim.tv_sec) * 1000000000 + info.st_mtim.tv_nsec)}};
}
} // namespace hm::web
