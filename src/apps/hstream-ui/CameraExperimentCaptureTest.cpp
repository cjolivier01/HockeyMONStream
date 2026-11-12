#include "src/apps/hstream-ui/CameraExperimentCaptureTest.h"
#include "src/apps/hstream-ui/CameraControlSpecs.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QRegularExpression>
#include <QtCore/QSaveFile>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <iostream>
#include <stdexcept>

namespace camera_capture_test {
namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}

QByteArray read_descriptor(int descriptor, qint64 limit) {
  struct stat before{}, after{};
  require(
      ::fstat(descriptor, &before) == 0 && S_ISREG(before.st_mode) && before.st_size <= limit,
      "Capture protocol file is not a bounded regular file");
  QByteArray bytes;
  char buffer[4096];
  while (bytes.size() <= limit) {
    const ssize_t count = ::read(descriptor, buffer, sizeof(buffer));
    if (count < 0 && errno == EINTR)
      continue;
    require(count >= 0, "Cannot read capture protocol file");
    if (!count)
      break;
    bytes.append(buffer, count);
  }
  require(
      bytes.size() <= limit && ::fstat(descriptor, &after) == 0 && before.st_size == after.st_size &&
          before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
          before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec,
      "Capture protocol file changed or exceeded its bound");
  return bytes;
}

QJsonObject object(const QByteArray& bytes) {
  QJsonParseError error;
  const auto document = QJsonDocument::fromJson(bytes, &error);
  require(error.error == QJsonParseError::NoError && document.isObject(), "Malformed capture protocol JSON");
  return document.object();
}

QString entry(int sequence, const char* kind) {
  return QString("%1-%2.json").arg(kind).arg(sequence, 6, 10, QLatin1Char('0'));
}

QSet<QString> control_ids() {
  // Values are unused: only the shared catalog's identifiers are inspected.
  const auto unused = [](const char*) { return 0; };
  QSet<QString> ids;
  for (const auto& group : {tracking_control_specs(unused), motion_control_specs(unused), player_control_specs(unused)})
    for (const auto& spec : group)
      ids.insert(QString::fromLatin1(spec.id));
  return ids;
}
} // namespace

Controls parse_controls(const QByteArray& bytes) {
  require(bytes.size() <= 65536, "Control snapshot is oversized");
  const auto input = object(bytes);
  require(
      input.size() == 3 && input.value("schema") == 1 && input.value("controls").isArray() &&
          input.value("playback_start_ms").isDouble(),
      "Control snapshot requires schema, controls and playback_start_ms");
  const double playback = input.value("playback_start_ms").toDouble();
  require(
      std::isfinite(playback) && playback >= 0 && playback <= 9007199254740991.0 && std::floor(playback) == playback,
      "Invalid control snapshot Playback start");
  Controls result;
  result.playback_start_ms = static_cast<qint64>(playback);
  const auto known = control_ids();
  for (const auto& item : input.value("controls").toArray()) {
    require(item.isArray(), "Control snapshot entries must be [identifier,value] arrays");
    const auto pair = item.toArray();
    require(
        pair.size() == 2 && pair[0].isString() && known.contains(pair[0].toString()) && pair[1].isDouble() &&
            std::isfinite(pair[1].toDouble()),
        "Unknown or invalid control snapshot entry");
    const QString id = pair[0].toString();
    const double value = pair[1].toDouble();
    if (id == "Apply_To_Fast_Box" || id == "Apply_To_Follower_Box")
      require(value == 0 || value == 1, "Camera target choices must use raw 0/1 values");
    require(result.values.emplace(id, value).second, "Duplicate control snapshot identifier");
  }
  result.complete = result.values.size() == static_cast<std::size_t>(known.size());
  return result;
}

Controls read_controls(const QString& path) {
  const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  require(descriptor >= 0, "Cannot open control snapshot");
  QByteArray bytes;
  try {
    bytes = read_descriptor(descriptor, 65536);
  } catch (...) {
    ::close(descriptor);
    throw;
  }
  ::close(descriptor);
  return parse_controls(bytes);
}

void write_json(const QString& path, const QJsonObject& value) {
  QSaveFile file(path);
  const auto bytes = QJsonDocument(value).toJson();
  require(
      file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
      "Cannot publish capture evidence");
}

Handshake::Handshake(const QString& directory) : directory_(directory) {
  if (directory.isEmpty())
    return;
  require(QFileInfo(directory).isAbsolute(), "Capture handshake directory must be absolute");
  descriptor_ = ::open(QFile::encodeName(directory).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  require(descriptor_ >= 0, "Capture handshake requires an existing private empty directory");
  try {
    struct stat info{};
    require(
        ::fstat(descriptor_, &info) == 0 && info.st_uid == ::geteuid() && (info.st_mode & 0777) == 0700,
        "Capture handshake directory must be owned by this user with mode 0700");
    device_ = info.st_dev;
    inode_ = info.st_ino;
    DIR* entries = ::fdopendir(::dup(descriptor_));
    require(entries != nullptr, "Cannot inspect capture handshake directory");
    bool empty = true;
    errno = 0;
    while (const auto* item = ::readdir(entries))
      if (QByteArray(item->d_name) != "." && QByteArray(item->d_name) != "..")
        empty = false;
    const int error = errno;
    ::closedir(entries);
    require(empty && error == 0, "Capture handshake directory must be new and empty");
    token_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
  } catch (...) {
    ::close(descriptor_);
    descriptor_ = -1;
    throw;
  }
}

Handshake::~Handshake() {
  if (descriptor_ >= 0)
    ::close(descriptor_);
}

void Handshake::check_directory() const {
  struct stat info{};
  require(
      ::lstat(QFile::encodeName(directory_).constData(), &info) == 0 && S_ISDIR(info.st_mode) &&
          info.st_dev == device_ && info.st_ino == inode_ && info.st_uid == ::geteuid() &&
          (info.st_mode & 0777) == 0700,
      "Capture handshake directory identity changed");
}

void Handshake::begin(const QString& phase, const QJsonObject& evidence) {
  if (!enabled())
    return;
  require(
      pending_phase_.isEmpty() && QRegularExpression("^[a-z0-9-]+$").match(phase).hasMatch() &&
          !phases_.contains(phase),
      "Invalid or repeated capture checkpoint");
  check_directory();
  phases_.insert(phase);
  pending_phase_ = phase;
  ++sequence_;
  write_json(
      QDir(directory_).filePath(entry(sequence_, "begin")),
      {{"schema", 1},
       {"run_token", token_},
       {"sequence", sequence_},
       {"phase", phase},
       {"evidence", evidence},
       {"status", "awaiting internal capture; outer owner watchdog required"}});
}

void Handshake::checkpoint(
    const QString& phase,
    const QJsonObject& evidence,
    const std::function<QJsonObject()>& stable_state,
    int timeout_ms) {
  if (!enabled())
    return;
  require(timeout_ms > 0 && timeout_ms <= 5000, "Invalid capture deadline");
  if (pending_phase_.isEmpty())
    begin(phase, evidence);
  require(pending_phase_ == phase, "Capture phase differs from its pre-capture marker");
  check_directory();
  const int sequence = sequence_;
  const QJsonObject identity{{"schema", 1}, {"run_token", token_}, {"sequence", sequence}, {"phase", phase}};
  auto request = identity;
  request.insert("evidence", evidence);
  const auto expected = stable_state();
  request.insert("stable_state", expected);
  write_json(QDir(directory_).filePath(entry(sequence, "request")), request);
  const QByteArray ack_name = QFile::encodeName(entry(sequence, "ack"));
  QElapsedTimer deadline;
  deadline.start();
  try {
    while (deadline.elapsed() < timeout_ms) {
      QCoreApplication::processEvents();
      check_directory();
      require(stable_state() == expected, "Camera checkpoint state changed while awaiting capture");
      const int descriptor =
          ::openat(descriptor_, ack_name.constData(), O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
      if (descriptor >= 0) {
        QByteArray bytes;
        try {
          bytes = read_descriptor(descriptor, 4096);
        } catch (...) {
          ::close(descriptor);
          throw;
        }
        ::close(descriptor);
        require(object(bytes) == identity, "Capture acknowledgment identity differs from the current checkpoint");
        require(deadline.elapsed() < timeout_ms, "Capture acknowledgment exceeded its deadline");
        auto result = identity;
        result.insert("status", "acknowledged");
        result.insert("wait_ms", deadline.elapsed());
        result.insert("scope", "Observer acknowledged durable capture; compositor pixels require separate review");
        write_json(QDir(directory_).filePath(entry(sequence, "result")), result);
        pending_phase_.clear();
        return;
      }
      require(errno == ENOENT, "Cannot open capture acknowledgment");
      QThread::msleep(1);
    }
    throw std::runtime_error("Capture acknowledgment timed out");
  } catch (const std::exception& error) {
    auto result = identity;
    result.insert("status", "failed");
    result.insert("message", error.what());
    result.insert("wait_ms", deadline.elapsed());
    check_directory();
    write_json(QDir(directory_).filePath(entry(sequence, "result")), result);
    throw;
  }
}

int confirmed_frame(const QString& label, int frame_count, bool screenshot_ready) {
  const auto match = QRegularExpression("(?:^|  ·  )frame ([0-9]+) / ([0-9]+)(?:$|  ·  )").match(label);
  bool valid_frame = false, valid_count = false;
  const int frame = match.captured(1).toInt(&valid_frame);
  const int count = match.captured(2).toInt(&valid_count);
  return screenshot_ready && match.hasMatch() && valid_frame && valid_count && count == frame_count && frame > 0 &&
          frame <= count
      ? frame - 1
      : -1;
}

QString seek_phase(int frame, int sequence) {
  require(frame >= 0 && sequence > 0, "Invalid visual timeline checkpoint identity");
  return QString("visual-timeline-seek-%1-frame-%2").arg(sequence).arg(frame);
}

QJsonObject capture_identity(QJsonObject value) {
  if (value.value("playing").toBool())
    for (const char* key :
         {"requested_index", "confirmed_index", "frame_label", "screenshot_ready", "trial_sample_id", "trial_pts_ns"})
      value.remove(key);
  // QWidget::grab can lay out a hidden subtree. Its cached bounds do not
  // describe presented pixels; retain visibility and every other state fence.
  const auto visibility = value.value("geometry_visibility").toObject();
  for (const char* key : {"preview", "timeline", "control_tabs"})
    if (visibility.contains(key) && visibility.value(key).isBool() && !visibility.value(key).toBool())
      value.remove(key);
  return value;
}

bool tests() {
  const auto rejects = [](const std::function<void()>& action) {
    try {
      action();
    } catch (const std::exception&) {
      return true;
    }
    return false;
  };
  const auto valid =
      QByteArray(R"({"schema":1,"controls":[["Max_Speed_X_x10",15],["Apply_To_Fast_Box",0]],"playback_start_ms":250})");
  const auto parsed = parse_controls(valid);
  require(
      parsed.values.at("Max_Speed_X_x10") == 15 && parsed.playback_start_ms == 250 && !parsed.complete,
      "Control snapshot did not preserve raw units");
  const QString frame_label = "Sample 104  ·  0.100 s  ·  frame 4 / 18000  ·  Program (12.0, 34.0) 480×270";
  require(confirmed_frame(frame_label, 18000, true) == 3, "Production Camera frame label was not recognized");
  for (const auto& label :
       {QString("Sample 104"),
        QString("frame 0 / 18000"),
        QString("frame 18001 / 18000"),
        QString("frame 4 / 17"),
        QString("frame 999999999999999999999 / 18000"),
        QString("unconfirmed frame 4 / 18000")})
    require(confirmed_frame(label, 18000, true) == -1, "Invalid Camera frame label was accepted");
  require(confirmed_frame(frame_label, 18000, false) == -1, "Covered Camera frame was treated as confirmed");
  QJsonArray all_controls;
  for (const auto& id : control_ids())
    all_controls.append(QJsonArray{id, 0});
  require(
      parse_controls(
          QJsonDocument(QJsonObject{{"schema", 1}, {"controls", all_controls}, {"playback_start_ms", 0}}).toJson())
          .complete,
      "Complete applicable control map was not recognized");
  for (const auto& invalid :
       {QByteArray(R"({"schema":1,"controls":[["Max_Speed_X_x10",15],["Max_Speed_X_x10",20]],"playback_start_ms":0})"),
        QByteArray(R"({"schema":1,"controls":[["unknown",1]],"playback_start_ms":0})"),
        QByteArray(R"({"schema":1,"controls":[["Max_Speed_X_x10",true]],"playback_start_ms":0})"),
        QByteArray(R"({"schema":1,"controls":[["Max_Speed_X_x10",1e999]],"playback_start_ms":0})"),
        QByteArray(R"({"schema":1,"controls":[["Apply_To_Fast_Box",2]],"playback_start_ms":0})"),
        QByteArray(R"({"schema":1,"controls":[],"playback_start_ms":-1})"),
        QByteArray(65537, ' ')})
    require(rejects([&] { parse_controls(invalid); }), "Invalid control snapshot was accepted");

  {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create repeated-seek capture directory");
    Handshake handshake(directory.path());
    int sequence = 0;
    for (const int frame : {0, 17999, 9000, 9000, 9000}) {
      const QString phase = seek_phase(frame, ++sequence);
      QTimer observer;
      observer.setSingleShot(true);
      QObject::connect(&observer, &QTimer::timeout, [&] {
        QFile request(QDir(directory.path()).filePath(entry(sequence, "request")));
        require(request.open(QIODevice::ReadOnly), "Repeated seek did not publish its request");
        auto ack = object(request.readAll());
        require(ack.value("phase").toString() == phase, "Repeated seek changed its phase");
        ack.remove("evidence");
        ack.remove("stable_state");
        write_json(QDir(directory.path()).filePath(entry(sequence, "ack")), ack);
      });
      observer.start(15);
      handshake.begin(phase, {});
      handshake.checkpoint(phase, {}, [&] { return QJsonObject{{"frame", frame}}; }, 400);
    }
  }

  for (const QString mode :
       {"success",
        "stale",
        "wrong-token",
        "wrong-sequence",
        "wrong-phase",
        "malformed",
        "oversized",
        "timeout",
        "changed"}) {
    QTemporaryDir directory;
    require(directory.isValid(), "Cannot create capture test directory");
    Handshake handshake(directory.path());
    int heartbeats = 0, state = 1;
    QTimer pulse, observer;
    QObject::connect(&pulse, &QTimer::timeout, [&] { ++heartbeats; });
    pulse.start(1);
    observer.setSingleShot(true);
    QObject::connect(&observer, &QTimer::timeout, [&] {
      if (mode == "timeout")
        return;
      if (mode == "changed") {
        state = 2;
        return;
      }
      QFile request(QDir(directory.path()).filePath("request-000001.json"));
      require(request.open(QIODevice::ReadOnly), "Observer did not see published request");
      auto ack = object(request.readAll());
      ack.remove("evidence");
      ack.remove("stable_state");
      if (mode == "wrong-token")
        ack["run_token"] = "wrong";
      if (mode == "wrong-sequence")
        ack["sequence"] = 0;
      if (mode == "wrong-phase")
        ack["phase"] = "previous";
      const auto name = mode == "stale" ? "ack-000000.json" : "ack-000001.json";
      QFile output(QDir(directory.path()).filePath(name));
      require(output.open(QIODevice::WriteOnly), "Cannot write observer fixture");
      output.write(
          mode == "malformed"       ? QByteArray("{")
              : mode == "oversized" ? QByteArray(4097, 'x')
                                    : QJsonDocument(ack).toJson());
      output.close();
    });
    observer.start(15);
    QElapsedTimer elapsed;
    elapsed.start();
    QString failure;
    try {
      handshake.checkpoint("test", {}, [&] { return QJsonObject{{"identity", state}}; }, 400);
    } catch (const std::exception& error) {
      failure = QString::fromUtf8(error.what());
    }
    const QString expected_failure = mode == "success" ? ""
        : mode == "stale" || mode == "timeout"         ? "timed out"
        : mode == "changed"                            ? "state changed"
        : mode == "malformed"                          ? "Malformed capture protocol JSON"
        : mode == "oversized"                          ? "bounded regular file"
                                                       : "identity differs";
    if ((mode == "success" ? !failure.isEmpty() : !failure.contains(expected_failure)) || heartbeats < 1 ||
        elapsed.elapsed() >= 3000 || ((mode == "stale" || mode == "timeout") && elapsed.elapsed() < 400))
      throw std::runtime_error(QString("Capture protocol regression %1: failure=%2 heartbeats=%3 elapsed=%4ms")
                                   .arg(mode)
                                   .arg(failure)
                                   .arg(heartbeats)
                                   .arg(elapsed.elapsed())
                                   .toStdString());
    require(rejects([&] { Handshake reused(directory.path()); }), "Nonempty handshake directory was reused");
  }
  std::cout
      << "PASS: production frame labels, repeated seek phases, bounded capture handoff, stale/wrong/malformed/oversized ACK, timeout, identity change, heartbeat, control snapshots\n";
  return true;
}
} // namespace camera_capture_test
