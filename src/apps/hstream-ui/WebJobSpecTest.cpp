#include "src/apps/hstream-ui/WebJobSpec.h"
#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
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
