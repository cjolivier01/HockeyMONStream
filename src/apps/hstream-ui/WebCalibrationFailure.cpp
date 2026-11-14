#include "src/apps/hstream-ui/WebCalibrationFailure.h"
#include "hstream/src/libs/stitching/CalibrationFailureProtocol.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <climits>
#include <cmath>
#include <regex>
#include <stdexcept>

namespace hm::web {
namespace {
namespace failure = hm::stitching::failure;
void require(bool value, const char* message) {
  if (!value)
    throw std::runtime_error(message);
}
void keys(const QJsonObject& object, std::initializer_list<const char*> names) {
  require(object.size() == qsizetype(names.size()), "Unexpected failure diagnosis fields");
  for (const auto name : names)
    require(object.contains(name), "Missing failure diagnosis field");
}
QString string(const QJsonObject& object, const char* key, int bound, bool empty = false) {
  const auto value = object.value(key);
  require(value.isString(), "Invalid failure diagnosis string");
  const auto text = value.toString();
  require(
      (empty || !text.isEmpty()) && text.size() <= bound && !text.contains(QChar(0)),
      "Failure diagnosis string exceeds its bound");
  return text;
}
void hash(const QJsonObject& object, const char* key) {
  require(failure::HexIdentity(string(object, key, 64).toStdString(), 64), "Invalid failure diagnosis digest");
}
qint64 integer(const QJsonObject& object, const char* key, qint64 lower, qint64 upper) {
  const auto value = object.value(key);
  const double number = value.toDouble();
  require(
      value.isDouble() && std::isfinite(number) && std::floor(number) == number && number >= lower && number <= upper,
      "Invalid failure diagnosis integer");
  return qint64(number);
}
bool same(const struct stat& a, const struct stat& b) {
  return a.st_dev == b.st_dev && a.st_ino == b.st_ino && a.st_size == b.st_size &&
      a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec &&
      a.st_ctim.tv_sec == b.st_ctim.tv_sec && a.st_ctim.tv_nsec == b.st_ctim.tv_nsec;
}
QString sha256(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace

QJsonObject DiagnoseCalibrationFailure(const Request& request, const std::function<bool()>& cancelled) {
  require(
      request.game == request.root / "game" && std::filesystem::is_empty(request.game),
      "Failure diagnosis requires an empty private helper game");
  require(
      request.runtime.isEmpty() && request.authorized_media.isEmpty(),
      "Failure diagnosis does not authorize runtime paths or media");
  keys(request.payload, {"version", "binding", "terminal", "log"});
  integer(request.payload, "version", 1, 1);
  const auto binding = request.payload.value("binding").toObject();
  keys(
      binding,
      {"parent_run_id",
       "native_run_id",
       "binding_token",
       "deployment_digest",
       "supervisor_request_sha256",
       "admission_digest",
       "workflow"});
  string(binding, "parent_run_id", 64);
  string(binding, "native_run_id", 64, true);
  for (const auto key : {"binding_token", "deployment_digest", "supervisor_request_sha256", "admission_digest"})
    hash(binding, key);
  if (!binding.value("workflow").isNull()) {
    require(binding.value("workflow").isObject(), "Invalid diagnostic workflow binding");
    const auto workflow = binding.value("workflow").toObject();
    keys(workflow, {"workflow_id", "stage_id", "attempt_id", "storage_namespace", "plan_digest"});
    for (const auto key : {"workflow_id", "stage_id", "attempt_id", "storage_namespace"})
      string(workflow, key, 160);
    hash(workflow, "plan_digest");
  }
  const auto terminal = request.payload.value("terminal").toObject();
  keys(terminal, {"returncode", "message", "cancelled"});
  const auto code = integer(terminal, "returncode", INT_MIN, INT_MAX);
  const auto message = string(terminal, "message", 4000, true);
  require(
      terminal.value("cancelled").isBool() && !terminal.value("cancelled").toBool() &&
          (code != 0 || !message.isEmpty()),
      "Failure diagnosis requires an original failed invocation");
  const auto log = request.payload.value("log").toObject();
  keys(log, {"path", "size", "sha256", "complete", "original_identity"});
  require(
      string(log, "path", 32) == "pipeline.log" && log.value("complete").isBool() && log.value("complete").toBool(),
      "Diagnosis requires the complete original pipeline log");
  const auto size = integer(log, "size", 1, failure::kMaximumLogBytes);
  hash(log, "sha256");
  const auto identity = log.value("original_identity").toObject();
  keys(identity, {"device", "inode", "size", "mtime_ns", "ctime_ns"});
  for (const auto key : {"device", "inode", "size", "mtime_ns", "ctime_ns"}) {
    const auto value = string(identity, key, 24).toStdString();
    require(std::regex_match(value, std::regex("0|[1-9][0-9]{0,23}")), "Invalid original diagnostic log identity");
  }
  require(identity.value("size").toString() == QString::number(size), "Original diagnostic log size changed");
  // Walk all root components without following aliases. The only authorized
  // input is the supervisor's private, exact-byte copy, never its original game.
  int descriptor = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  require(descriptor >= 0, "Cannot open diagnosis filesystem root");
  QFile root;
  require(root.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle), "Cannot own diagnostic root");
  for (const auto& component : request.root.relative_path()) {
    require(component != "." && component != "..", "Invalid diagnostic root");
    descriptor = ::openat(root.handle(), component.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    require(descriptor >= 0, "Cannot open diagnostic root without following links");
    root.close();
    require(
        root.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle), "Cannot own diagnostic directory");
  }
  descriptor = ::openat(root.handle(), "pipeline.log", O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  require(descriptor >= 0, "Cannot open complete diagnostic log");
  QFile input;
  require(input.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle), "Cannot own diagnostic log");
  struct stat before{}, after{}, named{};
  require(
      ::fstat(input.handle(), &before) == 0 && S_ISREG(before.st_mode) && before.st_size == size,
      "Diagnostic log identity changed");
  const auto bytes = input.read(failure::kMaximumLogBytes + 1);
  require(bytes.size() == size && sha256(bytes) == log.value("sha256").toString(), "Diagnostic log content changed");
  failure::Session session(binding.value("binding_token").toString().toStdString());
  qsizetype start = 0;
  while (start < bytes.size()) {
    require(!cancelled(), "cancelled");
    qsizetype end = bytes.indexOf('\n', start);
    if (end < 0)
      end = bytes.size();
    require(end - start <= qsizetype(failure::kMaximumLineBytes), "Diagnostic line exceeds its bound");
    const auto status = session.Record(std::string(bytes.constData() + start, std::size_t(end - start)));
    require(status.ok(), "Native failure context or evidence is invalid");
    start = end + 1;
  }
  require(session.has_context() && !session.invocation().empty(), "Native failure context is unavailable");
  require(
      ::fstat(input.handle(), &after) == 0 &&
          ::fstatat(root.handle(), "pipeline.log", &named, AT_SYMLINK_NOFOLLOW) == 0 && S_ISREG(named.st_mode) &&
          same(before, after) && same(before, named),
      "Diagnostic log changed during analysis");
  const auto diagnosis = session.Analyze(message.toStdString());
  QJsonArray evidence;
  for (const auto& line : diagnosis.evidence)
    evidence.append(QString::fromStdString(line));
  const auto record = session.records().find(diagnosis.cause_generation);
  const auto context_hash =
      record == session.records().end() ? QString() : sha256(QByteArray::fromStdString(record->second));
  const auto qs = [](const std::string& value) { return QString::fromStdString(value); };
  QJsonObject result{
      {"version", 1},
      {"analysis_version", 1},
      {"binding", binding},
      {"log", log},
      {"invocation", qs(session.invocation())},
      {"cause_generation", int(diagnosis.cause_generation)},
      {"context_record_sha256", context_hash},
      {"solve_outcome", qs(diagnosis.solve_outcome)},
      {"restart_outcome", qs(diagnosis.restart_outcome)},
      {"applicability", qs(diagnosis.applicability)},
      {"category", qs(diagnosis.category)},
      {"stage", qs(diagnosis.stage)},
      {"configuration", qs(diagnosis.configuration)},
      {"reported_cause", qs(diagnosis.reported_cause)},
      {"explanation", qs(diagnosis.explanation)},
      {"action", qs(diagnosis.action)},
      {"rendered_text", qs(diagnosis.rendered_text)},
      {"evidence", evidence},
      {"rejection_counts",
       QJsonObject{
           {"hypotheses", int(diagnosis.rejected_hypotheses)}, {"candidates", int(diagnosis.rejected_candidates)}}}};
  require(
      QJsonDocument(result).toJson(QJsonDocument::Compact).size() <= 128 * 1024,
      "Failure diagnosis exceeds its result bound");
  return result;
}
} // namespace hm::web
