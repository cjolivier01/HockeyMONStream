#include "src/apps/hstream-ui/WebJobSpec.h"
#include <sys/stat.h>
#include <unistd.h>
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
bool finalize_timestamp_on_sync = false;
bool mutate_timestamp_on_read = false;
void change_timestamp(int fd) {
  struct stat info {};
  if (::fstat(fd, &info) != 0)
    throw std::runtime_error("Cannot inspect timestamp fixture");
  struct timespec times[] = {info.st_atim, info.st_mtim};
  ++times[1].tv_sec;
  if (::futimens(fd, times) != 0)
    throw std::runtime_error("Cannot finalize timestamp fixture");
}
void require(bool value, const char* reason) {
  if (!value)
    throw std::runtime_error(reason);
}
template <typename Function>
void rejected(Function function) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error("Expected invalid request to be rejected");
}
} // namespace
extern "C" int __real_fsync(int fd);
extern "C" ssize_t __real_read(int fd, void* buffer, size_t size);
extern "C" int __wrap_fsync(int fd) {
  const int result = __real_fsync(fd);
  if (result == 0 && finalize_timestamp_on_sync) {
    finalize_timestamp_on_sync = false;
    change_timestamp(fd);
  }
  return result;
}
extern "C" ssize_t __wrap_read(int fd, void* buffer, size_t size) {
  const ssize_t result = __real_read(fd, buffer, size);
  if (result > 0 && mutate_timestamp_on_read) {
    mutate_timestamp_on_read = false;
    change_timestamp(fd);
  }
  return result;
}
int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  require(
      hm::web::Capabilities().value("features").toArray().contains("highlight-frame-inspection-v1"),
      "Authored frame inspection must be advertised before browser admission");
  QTemporaryDir temporary;
  require(temporary.isValid(), "Temporary directory unavailable");
  const fs::path root(temporary.path().toStdString());
  fs::create_directory(root / "game");
  const auto request_path = root / "operation-request.json";
  QJsonObject document{
      {"schema", 1},
      {"run_id", "test-run-1234"},
      {"operation", "matches-read"},
      {"root", temporary.path()},
      {"game_dir", "game"},
      {"payload", QJsonObject{}}};
  hm::web::WriteObject(request_path, document);
  const auto request = hm::web::LoadRequest(request_path);
  require(request.game == root / "game", "Game root was not resolved");
  rejected([&] { hm::web::Contained(root, "../escape"); });
  rejected([&] { hm::web::Contained(root, "/etc/passwd"); });
  fs::create_symlink("/etc", root / "escape");
  rejected([&] { hm::web::Contained(root, "escape/passwd"); });
  rejected([&] { hm::web::ReadObject(request_path, 10); });
  document.insert("operation", "arbitrary-shell");
  hm::web::WriteObject(request_path, document);
  rejected([&] { hm::web::LoadRequest(request_path); });
  document.insert("operation", "matches-read");
  document.insert("run_id", "../outside");
  hm::web::WriteObject(request_path, document);
  rejected([&] { hm::web::LoadRequest(request_path); });
  document.insert("run_id", "test-run-1234");
  document.insert("schema", 2);
  hm::web::WriteObject(request_path, document);
  rejected([&] { hm::web::LoadRequest(request_path); });
  hm::web::WriteObject(root / "result.json", {{"completed", true}});
  const auto artifact = hm::web::Artifact(request, root / "result.json", "metadata");
  require(artifact.value("path").toString() == "result.json", "Artifact path is not relative");
  require(artifact.value("size").toString().toLongLong() > 0, "Artifact bytes were not measured");
  require(artifact.value("mtime_ns").isString(), "Nanosecond identity would lose JSON precision");
  auto distributed = request;
  distributed.workflow = {
      {"workflow_id", "workflow-one"},
      {"stage_id", "stage-one"},
      {"attempt_id", "attempt-one"},
      {"plan_digest", QString(64, 'f')},
      {"storage_namespace", "shared-games"}};
  const auto portable = hm::web::Artifact(distributed, root / "result.json", "metadata");
  QFile bytes(QString::fromStdString((root / "result.json").string()));
  require(bytes.open(QIODevice::ReadOnly), "Cannot inspect portable fixture");
  const auto digest = QCryptographicHash::hash(bytes.readAll(), QCryptographicHash::Sha256).toHex();
  require(
      portable.value("identity_schema").toInt() == 2 && portable.value("size").isDouble() &&
          portable.value("storage_namespace").toString() == "shared-games" &&
          portable.value("sha256").toString() == QString::fromLatin1(digest) && !portable.contains("inode"),
      "Portable artifacts must bind bytes and namespace without host-local inode authority");
  finalize_timestamp_on_sync = true;
  const auto synchronized = hm::web::Artifact(distributed, root / "result.json", "metadata");
  require(
      !finalize_timestamp_on_sync && synchronized == portable,
      "Storage timestamp finalization must preserve the synchronized byte receipt");
  finalize_timestamp_on_sync = true;
  const auto synchronized_local = hm::web::Artifact(request, root / "result.json", "metadata");
  struct stat synchronized_info {};
  require(::stat((root / "result.json").c_str(), &synchronized_info) == 0, "Cannot inspect synchronized fixture");
  require(
      !finalize_timestamp_on_sync &&
          synchronized_local.value("mtime_ns").toString() ==
              QString::number(
                  qint64(synchronized_info.st_mtim.tv_sec) * 1000000000 + synchronized_info.st_mtim.tv_nsec),
      "Local artifacts must bind the synchronized file timestamps");
  mutate_timestamp_on_read = true;
  rejected([&] { hm::web::Artifact(distributed, root / "result.json", "metadata"); });
  require(!mutate_timestamp_on_read, "Mutation during hashing was not exercised");
  document.insert("schema", 1);
  document.insert("workflow", distributed.workflow);
  hm::web::WriteObject(request_path, document);
  require(hm::web::LoadRequest(request_path).workflow == distributed.workflow, "Admitted workflow binding was lost");
  auto invalid_binding = distributed.workflow;
  invalid_binding.insert("plan_digest", "not-a-digest");
  document.insert("workflow", invalid_binding);
  hm::web::WriteObject(request_path, document);
  rejected([&] { hm::web::LoadRequest(request_path); });
  fs::create_symlink(root / "result.json", root / "alias.json");
  rejected([&] { hm::web::Artifact(request, root / "alias.json", "metadata"); });
  std::ofstream(root / "empty").close();
  rejected([&] { hm::web::Artifact(request, root / "empty", "metadata"); });
  std::cout << "Workflow request confinement, identity and durable document tests passed\n";
}
