#pragma once

// Test-only observations. These helpers never modify live configuration, receipts
// or ownership, and a stopped QProcess alone is never retirement evidence.
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QProcessEnvironment>
#include <QtCore/QSaveFile>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <functional>
#include "src/apps/hstream-ui/SupervisedProcess.h"

namespace fresh_calibration_evidence {
inline QString timestamp() {
  return QString::number(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}
struct File {
  QByteArray bytes;
  QJsonObject metadata;
  bool valid() const {
    return metadata.value("state") == "captured";
  }
  bool absent() const {
    return metadata.value("state") == "absent";
  }
  QJsonObject json() const {
    auto result = metadata;
    if (valid())
      result["bytes_base64"] = QString::fromLatin1(bytes.toBase64());
    return result;
  }
};
inline File read(int fd, const QString& path, qint64 limit = 4 * 1024 * 1024) {
  File result;
  result.metadata = {{"path", path}, {"before_ns", timestamp()}, {"state", "unverified"}};
  struct stat before{}, after{};
  if (fd < 0 || ::fstat(fd, &before) || !S_ISREG(before.st_mode) || before.st_size < 0 || before.st_size > limit) {
    result.metadata["error"] = "not a bounded regular file";
    return result;
  }
  result.bytes.resize(before.st_size);
  qint64 offset = 0;
  while (offset < before.st_size) {
    const auto count = ::pread(fd, result.bytes.data() + offset, before.st_size - offset, offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      break;
    offset += count;
  }
  result.metadata["after_ns"] = timestamp();
  if (offset != before.st_size || ::fstat(fd, &after) || before.st_dev != after.st_dev ||
      before.st_ino != after.st_ino || before.st_size != after.st_size ||
      before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec) {
    result.bytes.clear();
    result.metadata["error"] = "file changed during bounded read";
    return result;
  }
  result.metadata["state"] = "captured";
  result.metadata["size"] = double(before.st_size);
  result.metadata["device"] = QString::number(before.st_dev);
  result.metadata["inode"] = QString::number(before.st_ino);
  result.metadata["sha256"] =
      QString::fromLatin1(QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256).toHex());
  return result;
}
inline File snapshot(const QString& path, qint64 limit = 4 * 1024 * 1024) {
  const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    File result;
    result.metadata = {
        {"path", path},
        {"state", errno == ENOENT ? "absent" : "unverified"},
        {"errno", errno},
        {"before_ns", timestamp()},
        {"after_ns", timestamp()}};
    return result;
  }
  auto result = read(fd, path, limit);
  ::close(fd);
  return result;
}
// Hash large immutable media/geometry without retaining their bytes. The caller
// supplies a shared phase deadline so nine artifacts cannot each extend it.
inline QJsonObject fingerprint(
    const QString& path,
    qint64 limit,
    std::chrono::steady_clock::time_point deadline,
    const std::function<std::chrono::steady_clock::time_point()>& now = std::chrono::steady_clock::now) {
  QJsonObject result{{"path", path}, {"state", "unverified"}, {"before_ns", timestamp()}};
  const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    result["errno"] = errno;
    return result;
  }
  struct stat before{}, after{};
  QCryptographicHash hash(QCryptographicHash::Sha256);
  bool valid = ::fstat(fd, &before) == 0 && S_ISREG(before.st_mode) && before.st_size >= 0 && before.st_size <= limit;
  QByteArray chunk(1024 * 1024, 0);
  qint64 offset = 0;
  while (valid && offset < before.st_size) {
    if (now() >= deadline) {
      valid = false;
      break;
    }
    const auto count = ::pread(fd, chunk.data(), std::min<qint64>(chunk.size(), before.st_size - offset), offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0) {
      valid = false;
      break;
    }
    hash.addData(QByteArrayView(chunk.constData(), count));
    offset += count;
  }
  valid = valid && ::fstat(fd, &after) == 0 && before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
      before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
      before.st_mtim.tv_nsec == after.st_mtim.tv_nsec && before.st_ctim.tv_sec == after.st_ctim.tv_sec &&
      before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
  ::close(fd);
  // The final pread/fstat (including an empty file) can cross the caller's
  // deadline. A pre-read check alone must not certify that late observation.
  valid = valid && now() < deadline;
  result["after_ns"] = timestamp();
  if (valid) {
    result["state"] = "captured";
    result["size"] = double(before.st_size);
    result["device"] = QString::number(before.st_dev);
    result["inode"] = QString::number(before.st_ino);
    result["sha256"] = QString::fromLatin1(hash.result().toHex());
  }
  return result;
}
inline bool same_content(const QJsonObject& first, const QJsonObject& second) {
  return first["state"] == "captured" && second["state"] == "captured" && first["sha256"] == second["sha256"] &&
      first["size"] == second["size"];
}
inline bool writeBytes(const QString& path, const QByteArray& bytes) {
  if (bytes.size() > 32 * 1024 * 1024 || !QDir().mkpath(QFileInfo(path).absolutePath()))
    return false;
  QSaveFile file(path);
  return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
}
inline bool write(const QString& path, const QJsonObject& object) {
  return writeBytes(path, QJsonDocument(object).toJson());
}
inline QJsonObject command(const QProcess& process) {
  const auto arguments = process.arguments();
  const int separator = arguments.indexOf("--");
  return {
      {"program", process.program()},
      {"arguments", QJsonArray::fromStringList(arguments)},
      {"working_directory", process.workingDirectory()},
      {"child_argv", QJsonArray::fromStringList(separator >= 0 ? arguments.mid(separator + 1) : QStringList())}};
}
class Observer {
 public:
  Observer(QProcess& process, QString config, std::function<QJsonObject()> identity)
      : process_(process), config_(std::move(config)), identity_(std::move(identity)) {
    state_ = QObject::connect(&process_, &QProcess::stateChanged, &scope_, [this](QProcess::ProcessState state) {
      if (state == QProcess::Starting) {
        ++launches;
        launch = command(process_);
        launch["identity"] = identity_();
        admitted = snapshot(config_);
        launch["config"] = admitted.json();
        const auto arguments = process_.arguments();
        const int separator = arguments.indexOf("--");
        const auto option = [&](const char* key) {
          const int index = arguments.indexOf(key);
          return index >= 0 && index + 1 < separator ? arguments[index + 1] : QString();
        };
        receipt_path_ = option("--receipt");
        launch["receipt"] = receipt_path_;
        launch["token"] = option("--token");
        launch["nonce"] = option("--nonce");
        if (receipt_fd_ >= 0)
          ::close(receipt_fd_);
        receipt_fd_ =
            ::open(QFile::encodeName(receipt_path_).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        launch["initial_receipt"] = read(receipt_fd_, receipt_path_, 16384).json();
      } else if (state == QProcess::NotRunning) {
        // stateChanged precedes the window's finished handler and owner release.
        // A bound C may validate before the adapter records its exit; requiring
        // adapter callback order here would reject genuine proof.
        const auto proof = hm::ui::SupervisedProcessStopped(process_);
        const auto receipt = read(receipt_fd_, receipt_path_, 16384);
        retirement = {
            {"validator_ok", proof.ok()},
            {"validator_stopped", proof.ok() && *proof},
            {"validator_status", QString::fromStdString(proof.status().ToString())},
            {"started", started},
            {"receipt", receipt.json()},
            {"identity", identity_()}};
        verified = started && launches == 1 && proof.ok() && *proof && receipt.valid() && admitted.valid() &&
            launch["initial_receipt"].toObject()["state"] == "captured";
        retirement["verified"] = verified;
      }
    });
    start_ = QObject::connect(&process_, &QProcess::started, &scope_, [this] {
      started = true;
      launch["started_identity"] = identity_();
      launch["pid"] = QString::number(process_.processId());
    });
  }
  ~Observer() {
    QObject::disconnect(state_);
    QObject::disconnect(start_);
    if (receipt_fd_ >= 0)
      ::close(receipt_fd_);
  }
  Observer(const Observer&) = delete;
  Observer& operator=(const Observer&) = delete;
  bool commandUnchanged() const {
    const auto current = command(process_);
    for (const auto& key : current.keys())
      if (current[key] != launch[key])
        return false;
    return launches == 1;
  }
  QJsonObject launch, retirement;
  File admitted;
  int launches{0};
  bool started{false}, verified{false};

 private:
  QProcess& process_;
  QString config_, receipt_path_;
  std::function<QJsonObject()> identity_;
  QObject scope_;
  QMetaObject::Connection state_, start_;
  int receipt_fd_{-1};
};

// Pure, early validation: call before QApplication, fixture preparation or probes.
inline QString flags(const QProcessEnvironment& env) {
  const auto has = [&](const char* name) { return env.contains(name); };
  const bool unsaved = has("HSTREAM_UI_E2E_FRESH_UNSAVED");
  if (has("HSTREAM_UI_E2E_ACTIVE_SAVE") && !unsaved)
    return "ACTIVE_SAVE requires FRESH_UNSAVED";
  if (!unsaved)
    return {};
  for (const char* name :
       {"HSTREAM_UI_E2E_FRESH_UNSAVED",
        "HSTREAM_UI_E2E_FRESH_CALIBRATION",
        "HSTREAM_UI_E2E_CALIBRATION_COMPLETION_ONLY"})
    if (env.value(name) != "1")
      return QString("Fresh unsaved requires %1=1").arg(name);
  if (has("HSTREAM_UI_E2E_ACTIVE_SAVE") && env.value("HSTREAM_UI_E2E_ACTIVE_SAVE") != "1")
    return "ACTIVE_SAVE must equal 1";
  for (const auto& key : env.keys())
    if (key.startsWith("HSTREAM_UI_TEST_") ||
        (key.startsWith("HSTREAM_UI_E2E_") && key.endsWith("_ONLY") &&
         key != "HSTREAM_UI_E2E_CALIBRATION_COMPLETION_ONLY") ||
        key == "HSTREAM_UI_E2E_PREPARE_GAME" || key == "HSTREAM_UI_E2E_CLEAN_CALIBRATION" ||
        key == "HSTREAM_UI_E2E_SAVED_LEVELING_POINTS" || key == "HSTREAM_UI_E2E_AMD_PROGRAM")
      return "Fresh unsaved rejects incompatible E2E mode: " + key;
  if (env.value("HSTREAM_UI_E2E_RUN_MODE") != "stitch-calibration")
    return "Fresh unsaved requires stitch-calibration mode";
  bool valid = false;
  const int reference = env.value("HSTREAM_UI_E2E_REFERENCE_MS").toInt(&valid);
  if (!valid || reference <= 0 || reference >= 86400000)
    return "Fresh unsaved requires an explicit positive reference";
  const int timeout = env.value("HSTREAM_UI_E2E_TIMEOUT_MS").toInt(&valid);
  if (!valid || timeout <= 0 || timeout > 4500000)
    return "Fresh unsaved requires a bounded explicit timeout";
  const auto game = env.value("HSTREAM_UI_E2E_GAME_ID");
  const auto root = env.value("HM_GAME_DIR");
  const auto output = env.value("HSTREAM_UI_E2E_ARTIFACT_DIR");
  if (game.isEmpty() || game == "." || game == ".." || game.contains('/') || !QFileInfo(root).isAbsolute() ||
      !QFileInfo(output).isAbsolute() || QFileInfo::exists(QDir(root).filePath(game)) || QFileInfo::exists(output))
    return "Fresh unsaved requires a new game and absolute new evidence directory";
  if (QDir::cleanPath(output) == QDir::cleanPath(QDir(root).filePath(game)) ||
      QDir::cleanPath(output).startsWith(QDir::cleanPath(QDir(root).filePath(game)) + "/"))
    return "Evidence must be outside the game";
  for (const char* key : {"HSTREAM_UI_E2E_FRESH_LEFT", "HSTREAM_UI_E2E_FRESH_RIGHT"})
    if (!QFileInfo(env.value(key)).isFile())
      return "Fresh unsaved requires two real source files";
  if (QFileInfo(env.value("HSTREAM_UI_E2E_FRESH_LEFT")).canonicalFilePath() ==
      QFileInfo(env.value("HSTREAM_UI_E2E_FRESH_RIGHT")).canonicalFilePath())
    return "Fresh unsaved requires distinct physical Left/Right recordings";
  return {};
}
} // namespace fresh_calibration_evidence
