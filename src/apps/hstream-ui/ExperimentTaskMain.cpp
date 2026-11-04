#include "hstream/src/libs/common/GameOperationOwner.h"
#include "hstream/src/libs/common/GameStorage.h"
#include "hstream/src/libs/stitching/HuginProject.h"
#include "hstream/src/libs/stitching/TransactionState.h"
#include "src/apps/hstream-ui/ExperimentTaskRuntime.h"
#include "src/apps/hstream-ui/StitchingExperimentStore.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QJsonDocument>
#include <QtCore/QProcess>
#include <QtCore/QProcessEnvironment>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <iostream>
#include <regex>
#include <set>
#include <stdexcept>

namespace fs = std::filesystem;
namespace {
using namespace hm::web;
namespace admission = hm::game_operation;
volatile sig_atomic_t interrupted = 0;
void stop(int) {
  interrupted = 1;
}
void require(bool value, const std::string& message) {
  if (!value)
    throw std::runtime_error(message);
}
void check(const absl::Status& status) {
  require(status.ok(), status.ToString());
}
template <class T>
T take(absl::StatusOr<T> value) {
  check(value.status());
  return std::move(*value);
}
QString text(const std::string& value) {
  return QString::fromStdString(value);
}
QString path(const fs::path& value) {
  return text(value.string());
}
std::string identity(const QJsonObject& object, const char* key) {
  const auto value = RequiredString(object, key, 160).toStdString();
  require(std::regex_match(value, std::regex("[A-Za-z0-9_.:-]{1,160}")), "Invalid identity field " + std::string(key));
  return value;
}
fs::path absolute_directory(const QJsonObject& object, const char* key) {
  const fs::path result(RequiredString(object, key, 4096).toStdString());
  require(
      result.is_absolute() && result == result.lexically_normal() && fs::is_directory(result),
      "Expected normalized absolute directory " + std::string(key));
  require(fs::canonical(result) == result, "Directory aliases are forbidden in workflow authority");
  return result;
}
QJsonValue owner_json(const std::optional<admission::Owner>& owner) {
  if (!owner)
    return QJsonValue(QJsonValue::Null);
  return QJsonObject{
      {"kind", text(owner->kind)},
      {"id", text(owner->id)},
      {"token", text(owner->token)},
      {"host", text(owner->host)},
      {"boot_id", text(owner->boot_id)},
      {"process_id", double(owner->process_id)},
      {"revision", double(owner->revision)},
      {"cancel_requested", owner->cancel_requested},
      {"workflow_directory", text(owner->workflow_directory)}};
}
uint64_t revision(const QJsonObject& request) {
  const auto value = Number(request, "revision", 1, 9007199254740991.0, 0);
  require(value > 0 && value == std::floor(value), "Expected exact owner revision");
  return static_cast<uint64_t>(value);
}
admission::Owner new_owner(const QJsonObject& request, const fs::path& game) {
  auto result = admission::LocalOwner(identity(request, "kind"), identity(request, "id"), identity(request, "token"));
  require(result.kind == "workflow", "Workflow admission requires a workflow owner");
  result.workflow_directory = absolute_directory(request, "workflow_directory").string();
  require(
      fs::path(result.workflow_directory) == game / "_hockeymon/workflows" / result.id,
      "Workflow admission directory does not identify this game and workflow");
  return result;
}
QJsonObject workspace_json(const StitchingExperimentWorkspace& value) {
  return {
      {"root", path(value.root)},
      {"game_directory", path(value.game_directory)},
      {"game_id", text(value.game_id)},
      {"invalidation_id", text(value.invalidation_id)},
      {"settings", hm::experiment::settings_json(value.settings)}};
}
StitchingExperimentWorkspace workspace(const QJsonObject& object, const fs::path& game, const fs::path& workflow) {
  StitchingExperimentWorkspace value;
  value.root = absolute_directory(object, "root");
  value.game_directory = absolute_directory(object, "game_directory");
  value.game_id = identity(object, "game_id");
  value.invalidation_id = identity(object, "invalidation_id");
  require(value.game_directory == value.root / value.game_id, "Workspace game identity mismatch");
  bool contained = false;
  for (const auto& allowed : {workflow, game / "_hockeymon/stitching-experiments/sessions"}) {
    const auto relative = value.game_directory.lexically_relative(allowed);
    contained |= !relative.empty() && !relative.is_absolute() && *relative.begin() != "..";
  }
  std::optional<StitchingExperimentWorkspace> retained;
  if (!contained && fs::exists(game / "_hockeymon/stitching-experiments")) {
    const auto store = take(OpenStitchingExperimentStore(game));
    const auto catalog = take(LoadStitchingExperimentStore(store));
    for (const auto& row : catalog.experiments) {
      if (row.workflow_active)
        continue;
      for (const auto& reference : {row.result_workspace, row.selection_workspace}) {
        if (reference && reference->root == value.root && reference->game_directory == value.game_directory &&
            reference->game_id == value.game_id && reference->invalidation_id == value.invalidation_id) {
          retained = *reference;
          contained = true;
        }
      }
    }
  }
  require(contained, "Workspace is outside this workflow and the selected game's retained catalog inputs");
  const auto config_text = take(
      hm::stitching::read_bounded_regular_file_no_follow(
          value.game_directory / "_hockeymon/config.yaml", 4 * 1024 * 1024, "experiment config"));
  const auto config = YAML::Load(config_text);
  require(
      config["hstream_ui"]["stitching_calibration"]["invalidation_id"].as<std::string>("") == value.invalidation_id,
      "Workspace generation changed");
  require(object.value("settings").isObject(), "Workspace requires frozen settings");
  value.settings = hm::experiment::settings(object.value("settings").toObject(), config);
  const auto manual = object.value("settings").toObject().value("manual_control_points").toString();
  if (!manual.isEmpty())
    value.settings.manual_control_points = manual.toStdString();
  if (retained)
    require(
        hm::experiment::settings_json(value.settings) == hm::experiment::settings_json(retained->settings),
        "Retained workflow settings do not match the exact catalog reference");
  return value;
}
void verify_owner(const fs::path& game, const std::string& token, const std::string& workflow = {}) {
  auto lock = take(admission::Lock::Acquire(game));
  auto owner = take(lock->Read());
  require(
      owner && owner->token == token && owner->kind == "workflow" && (workflow.empty() || owner->id == workflow),
      "ABORTED: Workflow game ownership changed");
  check(lock->Authorize(token));
}
StoredStitchingExperiment find_row(const StitchingExperimentStore& store, const std::string& key) {
  const auto catalog = take(LoadStitchingExperimentStore(store));
  for (const auto& row : catalog.experiments)
    if (row.workspace.game_directory.lexically_relative(store.directory).generic_string() == key)
      return row;
  throw std::runtime_error("NOT_FOUND: Workflow candidate is absent from the experiment catalog");
}
QJsonObject admission_command(const std::string& action, const QJsonObject& request) {
  const auto game = absolute_directory(request, "game");
  if (action == "bind" || action == "adopt" || action == "adopt-selection" || action == "adoption-status") {
    const auto token = identity(request, "token"), workflow = identity(request, "workflow_id");
    const auto directory = absolute_directory(request, "workflow_directory");
    require(directory == game / "_hockeymon/workflows" / workflow, "Invalid workflow directory");
    verify_owner(game, token, workflow);
    admission::RegisterAuthorization(game, token);
    const auto store = action == "adoption-status"
        ? StitchingExperimentStore{game / "_hockeymon/stitching-experiments", game}
        : take(OpenStitchingExperimentStore(game));
    if (action == "bind") {
      require(request.value("catalog_keys").isArray(), "Workflow bind requires catalog keys");
      const auto keys = request.value("catalog_keys").toArray();
      require(keys.size() <= int(kMaximumStoredStitchingExperiments), "Too many workflow candidates");
      std::vector<StoredStitchingExperiment> rows;
      std::set<std::string> unique;
      for (const auto key : keys) {
        require(key.isString() && unique.insert(key.toString().toStdString()).second, "Invalid duplicate catalog key");
        auto row = find_row(store, key.toString().toStdString());
        require(
            row.process_session_id == 0 && row.process_token.empty() &&
                (row.workflow_id.empty() || row.workflow_id == workflow),
            "Candidate has another execution owner");
        require(row.state != "quarantined", "Candidate process containment is unconfirmed");
        row.workflow_id = workflow;
        row.workflow_directory = directory;
        row.workflow_active = true;
        if (!row.result_workspace)
          row.state = "queued";
        rows.push_back(std::move(row));
      }
      for (auto& row : rows)
        check(SaveStitchingExperiment(store, row));
      return {{"bound", true}};
    }
    const auto key = RequiredString(request, "catalog_key", 520).toStdString();
    auto row = find_row(store, key);
    require(row.workflow_id == workflow && row.workflow_active, "Candidate is not owned by this workflow");
    const auto attempt = identity(request, "attempt_id");
    if (action == "adoption-status") {
      const auto adoption = RequiredString(request, "adoption", 32).toStdString();
      require(adoption == "adopt" || adoption == "adopt-selection", "Invalid adoption status kind");
      const bool selection = adoption == "adopt-selection";
      const auto& saved = selection ? row.selection_workspace : row.result_workspace;
      const auto& saved_attempt = selection ? row.selection_attempt_id : row.result_attempt_id;
      if (!saved)
        return {{"adopted", false}};
      // The durable catalog is the commit acknowledgment. Recovery must not
      // reopen output PNGs or attempt another mutation after cancellation.
      require(
          saved_attempt == attempt && request.value("workspace").isObject() &&
              workspace_json(*saved) == request.value("workspace").toObject(),
          "A different workflow result was already adopted");
      return {{"adopted", true}};
    }
    const auto result = workspace(request.value("workspace").toObject(), game, directory);
    const auto relative = result.game_directory.lexically_relative(directory / "attempts" / attempt);
    require(
        !relative.empty() && !relative.is_absolute() && *relative.begin() != "..", "Result escapes its exact attempt");
    if (action == "adopt-selection") {
      require(hm::experiment::materialized(result), "Selected input bundle is incomplete");
      const auto fingerprint =
          take(ReusableStitchingExperimentSelectionFingerprint(result.game_directory, result.settings));
      require(!fingerprint.empty(), "Selection adoption requires frozen frame identity");
      if (row.selection_workspace) {
        require(
            row.selection_attempt_id == attempt && row.selection_workspace->game_directory == result.game_directory &&
                row.selection_workspace->invalidation_id == result.invalidation_id &&
                row.selection_fingerprint == fingerprint,
            "A different selected-input bundle was already adopted");
      } else {
        row.selection_workspace = result;
        row.selection_attempt_id = attempt;
        row.selection_fingerprint = fingerprint;
        row.state = "frozen";
        row.failure.clear();
        check(SaveStitchingExperiment(store, row, true));
      }
      return {
          {"adopted", true},
          {"catalog_key", text(key)},
          {"revision", double(row.revision)},
          {"workspace", workspace_json(result)},
          {"attempt_id", text(attempt)},
          {"adoption", "selection"}};
    }
    if (row.result_workspace) {
      require(
          row.result_attempt_id == attempt && row.result_workspace->game_directory == result.game_directory &&
              row.result_workspace->invalidation_id == result.invalidation_id,
          "A different result was already adopted");
    }
    check(CompleteStitchingExperimentWorkspace(result, request.value("require_ice_mask").toBool(false)));
    auto artifacts = take(hm::stitching::HuginProject::RecoverAndLock(result.game_directory));
    const auto generation = take(hm::stitching::HuginProject::GenerationId(result.game_directory, *artifacts));
    artifacts.reset();
    if (!row.result_workspace) {
      row.result_workspace = result;
      row.result_attempt_id = attempt;
      row.artifact_generation_id = generation;
      row.selection_fingerprint =
          take(ReusableStitchingExperimentSelectionFingerprint(result.game_directory, result.settings));
      row.state = "complete";
      row.failure.clear();
      check(SaveStitchingExperiment(store, row, !row.selection_fingerprint.empty()));
    } else
      require(row.artifact_generation_id == generation, "Adopted result generation changed");
    return {
        {"adopted", true},
        {"catalog_key", text(key)},
        {"revision", double(row.revision)},
        {"workspace", workspace_json(result)},
        {"attempt_id", text(attempt)}};
  }
  // Release retires catalog activity before removing the durable game claim.
  // Retrying after a response loss is harmless: already-retired rows are skipped.
  if (action == "release") {
    const auto token = identity(request, "token");
    require(
        request.value("stopped_and_clean").isBool() && request.value("stopped_and_clean").toBool(),
        "FAILED_PRECONDITION: Shutdown and cleanup proof is required");
    auto current = take(admission::Inspect(game));
    if (!current)
      return {{"owner", QJsonValue(QJsonValue::Null)}};
    require(current->token == token && current->revision == revision(request), "ABORTED: Game admission changed");
    admission::RegisterAuthorization(game, token);
    if (fs::exists(game / "_hockeymon/stitching-experiments")) {
      const auto store = take(OpenStitchingExperimentStore(game));
      const auto catalog = take(LoadStitchingExperimentStore(store));
      for (auto row : catalog.experiments) {
        if (!row.workflow_active || row.workflow_id != current->id)
          continue;
        row.workflow_active = false;
        if (!row.result_workspace) {
          row.state = "failed";
          row.failure = current->cancel_requested ? "Workflow cancelled after verified cleanup"
                                                  : "Workflow stopped without an adopted result";
        }
        check(SaveStitchingExperiment(store, row));
      }
    }
    auto lock = take(admission::Lock::Acquire(game));
    check(lock->Release(token, revision(request), true));
    return {{"owner", QJsonValue(QJsonValue::Null)}};
  }
  auto lock = take(admission::Lock::Acquire(game));
  if (action == "inspect")
    return {{"owner", owner_json(take(lock->Read()))}};
  if (action == "acquire") {
    auto owner = new_owner(request, game);
    const auto current = take(lock->Read());
    if (current && current->kind == owner.kind && current->id == owner.id && current->token == owner.token &&
        current->workflow_directory == owner.workflow_directory)
      return {{"owner", owner_json(current)}};
    check(lock->Claim(owner));
  } else if (action == "handoff") {
    auto successor = new_owner(request.value("successor").toObject(), game);
    check(lock->Handoff(successor, identity(request, "token"), revision(request)));
  } else if (action == "cancel") {
    check(lock->RequestCancel(identity(request, "token"), revision(request)));
  } else
    throw std::runtime_error("Invalid admission action");
  return {{"owner", owner_json(take(lock->Read()))}};
}
void run_process(
    const QString& program,
    const QStringList& arguments,
    const QString& directory,
    const QProcessEnvironment& environment) {
  require(!interrupted, "Task cancelled");
  QProcess child;
  child.setWorkingDirectory(directory);
  child.setProcessEnvironment(environment);
  child.setProcessChannelMode(QProcess::ForwardedChannels);
  child.start(program, arguments);
  require(child.waitForStarted(5000), "Cannot start native experiment process");
  // The allocation supervisor contains the task and all descendants. Cancellation
  // is its responsibility; this process never claims containment from a PID wait.
  while (child.state() != QProcess::NotRunning) {
    child.waitForFinished(100);
    if (interrupted)
      child.terminate();
  }
  require(
      !interrupted && child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
      "Native experiment failed or was cancelled; inspect attempt log");
}
QJsonObject execute_task(const QJsonObject& document) {
  require(document.value("schema").toInt() == 1, "Unsupported experiment task schema");
  const auto game = absolute_directory(document, "game");
  const auto root = absolute_directory(document, "root");
  const auto workflow = identity(document, "workflow_id"), stage = identity(document, "stage_id"),
             attempt = identity(document, "attempt_id");
  const auto directory = game / "_hockeymon/workflows" / workflow;
  require(root == directory / "attempts" / attempt, "Task root does not identify its exact attempt");
  const auto digest = RequiredString(document, "plan_digest", 64).toStdString();
  require(std::regex_match(digest, std::regex("[a-f0-9]{64}")), "Invalid plan digest");
  const auto owner = document.value("owner").toObject();
  require(absolute_directory(owner, "game") == game, "Task owner belongs to another game");
  verify_owner(game, identity(owner, "token"), workflow);
  admission::RegisterAuthorization(game, identity(owner, "token"));
  Request request;
  request.root = root;
  request.game = game;
  request.run_id = attempt;
  request.operation = RequiredString(document, "operation", 64).toStdString();
  require(
      document.value("inputs").isObject() && document.value("runtime").isObject(),
      "Task requires typed inputs and runtime");
  request.payload = document.value("inputs").toObject();
  request.runtime = document.value("runtime").toObject();
  const auto namespace_id = identity(request.runtime, "storage_namespace");
  request.workflow = {
      {"workflow_id", text(workflow)},
      {"stage_id", text(stage)},
      {"attempt_id", text(attempt)},
      {"plan_digest", text(digest)},
      {"storage_namespace", text(namespace_id)}};
  const std::set<std::string> operations{
      "experiment-create",
      "experiment-baseline",
      "experiment-scan",
      "experiment-materialize",
      "experiment-run",
      "experiment-reuse-selection",
      "experiment-complete"};
  require(operations.count(request.operation), "Unsupported experiment task kind");
  const bool gpu = request.operation == "experiment-baseline" || request.operation == "experiment-scan" ||
      request.operation == "experiment-materialize" || request.operation == "experiment-run";
  if (gpu) {
#ifdef HSTREAM_USE_HIP
    throw std::runtime_error(
        "UNIMPLEMENTED: AMD automatic calibration, player scanning and frame materialization are unavailable");
#endif
    require(!qEnvironmentVariableIsEmpty("CUDA_VISIBLE_DEVICES"), "GPU tasks require a qualified NVIDIA allocation");
    require(!qEnvironmentVariable("CUDA_VISIBLE_DEVICES").contains(','), "This task requires one allocated GPU");
  }
  const auto read_workspace = [&](const char* key) {
    require(request.payload.value(key).isObject(), "Missing workspace input " + std::string(key));
    return workspace(request.payload.value(key).toObject(), game, directory);
  };
  StitchingExperimentWorkspace candidate;
  bool available = true;
  std::string diagnostic;
  if (request.operation == "experiment-create" && request.payload.contains("workspace")) {
    candidate = take(SnapshotStitchingExperimentWorkspace(read_workspace("workspace"), root / "work/candidate"));
  } else if (request.operation == "experiment-create") {
    fs::path source = game;
    if (request.payload.contains("source_workspace"))
      source = read_workspace("source_workspace").game_directory;
    const auto config = YAML::LoadFile((source / "_hockeymon/config.yaml").string());
    auto settings = hm::experiment::settings(request.payload.value("settings").toObject(), config);
    const auto sequence = Number(request.payload, "sequence", 1, 1024, 1);
    require(sequence == std::floor(sequence), "Invalid experiment sequence");
    if (source == game)
      candidate = take(CreateStitchingExperimentWorkspace(source, root / "work/candidate", settings, int(sequence)));
    else {
      std::vector<fs::path> media;
      for (const auto& entry : fs::recursive_directory_iterator(source))
        if (entry.is_symlink())
          media.push_back(fs::canonical(entry.path()));
      candidate = take(CreateStagedStitchingExperimentWorkspace(
          source, root / "work/candidate", settings, int(sequence), media, false));
    }
  } else {
    const auto source = read_workspace("workspace");
    candidate = take(SnapshotStitchingExperimentWorkspace(source, root / "work/candidate"));
    if (request.operation == "experiment-scan") {
      const auto baseline =
          take(SnapshotStitchingExperimentWorkspace(read_workspace("baseline_workspace"), root / "work/baseline"));
      const auto selection = ScanExperimentFrames(request, baseline, candidate, run_process);
      available = selection.available;
      diagnostic = selection.diagnostic;
    } else if (request.operation == "experiment-reuse-selection") {
      check(ReuseStitchingExperimentPlayerSelection(read_workspace("selection_workspace"), candidate));
    } else if (request.operation == "experiment-complete") {
      check(CompleteStitchingExperimentWorkspace(candidate, request.payload.value("require_ice_mask").toBool(false)));
    } else {
      if (request.operation == "experiment-baseline")
        request.payload.insert("require_ice_mask", true);
      hm::experiment::run_experiment(
          request,
          candidate,
          run_process,
          request.operation != "experiment-materialize",
          request.operation == "experiment-materialize");
    }
  }
  require(!interrupted, "Task cancelled before publication");
  verify_owner(game, identity(owner, "token"), workflow);
  const auto selection =
      take(ReusableStitchingExperimentSelectionFingerprint(candidate.game_directory, candidate.settings));
  QJsonObject outputs{
      {"workspace", workspace_json(candidate)},
      {"selection_available", available && !selection.empty()},
      {"selection_fingerprint", text(selection)},
      {"materialized", hm::experiment::materialized(candidate)},
      {"inputs_available", hm::experiment::materialized(candidate)}};
  const bool complete = request.operation == "experiment-baseline" || request.operation == "experiment-run" ||
      request.operation == "experiment-complete";
  if (request.payload.contains("catalog_key") && (complete || request.operation == "experiment-materialize")) {
    outputs.insert("catalog_key", request.payload.value("catalog_key"));
    outputs.insert("adoption", complete ? "complete" : "selection");
  }
  const auto metadata = root / "native-outputs.json";
  WriteObject(metadata, outputs);
  QJsonArray artifacts{Artifact(request, metadata, "metadata")};
  size_t count = 0;
  for (const auto& entry : fs::recursive_directory_iterator(candidate.game_directory / "_hockeymon")) {
    require(++count <= 20000, "Task output exceeds file bound");
    const auto name = entry.path().filename().string();
    if (!entry.is_regular_file() || entry.is_symlink() || entry.file_size() == 0 || name.front() == '.' ||
        name == "manifest.json")
      continue;
    artifacts.append(Artifact(request, entry.path(), "calibration"));
  }
  QJsonObject result{
      {"schema", 1},
      {"workflow_id", text(workflow)},
      {"plan_digest", text(digest)},
      {"stage_id", text(stage)},
      {"attempt_id", text(attempt)},
      {"status", available ? "succeeded" : "unavailable"},
      {"outputs", outputs},
      {"artifacts", artifacts},
      {"message", text(diagnostic)}};
  const fs::path destination(RequiredString(document, "result_path", 4096).toStdString());
  require(destination == root / "native-result.json", "Task result path must be its exact native receipt");
  WriteObject(destination, result);
  return result;
}
} // namespace
int main(int argc, char** argv) {
  QCoreApplication application(argc, argv);
  std::signal(SIGTERM, stop);
  std::signal(SIGINT, stop);
  try {
    QJsonObject result;
    if (argc == 2 && std::string(argv[1]) == "--capabilities") {
      result = {
          {"schema", 1},
          {"request_schema", 1},
          {"receipt_schema", 1},
          {"features",
           QJsonArray{
               "experiment-materialization-v1", "workflow-portable-artifacts-v2", "game-operation-admission-v1"}},
          {"operations",
           QJsonArray{
               "experiment-create",
               "experiment-baseline",
               "experiment-scan",
               "experiment-materialize",
               "experiment-run",
               "experiment-reuse-selection",
               "experiment-complete"}},
          {"admission",
           QJsonArray{
               "acquire",
               "inspect",
               "handoff",
               "bind",
               "adopt",
               "adopt-selection",
               "adoption-status",
               "cancel",
               "release"}}};
#ifdef HSTREAM_USE_HIP
      result.insert("gpu_backend", "amd");
      result.insert("gpu_operations_supported", false);
#else
      result.insert("gpu_backend", "nvidia");
      result.insert("gpu_operations_supported", true);
#endif
    } else if (argc == 5 && std::string(argv[1]) == "--admission" && std::string(argv[3]) == "--request")
      result = admission_command(argv[2], ReadObject(argv[4]));
    else if (argc == 3 && std::string(argv[1]) == "--request")
      result = execute_task(ReadObject(argv[2]));
    else
      throw std::runtime_error("usage: hstream-experiment-task --request FILE | --admission ACTION --request FILE");
    std::cout << QJsonDocument(result).toJson(QJsonDocument::Compact).constData() << '\n';
    return 0;
  } catch (const std::exception& error) {
    const std::string message = error.what();
    std::string code = "invalid";
    if (message.find("ALREADY_EXISTS") != std::string::npos)
      code = "conflict";
    else if (message.find("ABORTED") != std::string::npos)
      code = "stale";
    else if (message.find("FAILED_PRECONDITION") != std::string::npos)
      code = "precondition";
    std::cout << QJsonDocument(QJsonObject{{"error", text(message)}, {"code", text(code)}})
                     .toJson(QJsonDocument::Compact)
                     .constData()
              << '\n';
    return interrupted ? 130 : 1;
  }
}
