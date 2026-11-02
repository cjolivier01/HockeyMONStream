#include "src/apps/hstream-ui/StitchingWorkflow.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/GameOperationOwner.h"
#include "src/apps/hstream-ui/StitchingExperimentDialog.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtGui/QWheelEvent>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition)
    throw std::runtime_error(message);
}
void write(const QString& path, const QByteArray& bytes) {
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "Cannot open fixture file");
  require(file.write(bytes) == bytes.size(), "Cannot write fixture file");
}
QJsonObject read(const QString& path) {
  QFile file(path);
  require(file.open(QIODevice::ReadOnly), "Cannot read fixture");
  return QJsonDocument::fromJson(file.readAll()).object();
}
bool wait(const std::function<bool()>& predicate, int maximum = 5000) {
  QElapsedTimer timer;
  timer.start();
  while (timer.elapsed() < maximum) {
    QApplication::processEvents();
    if (predicate())
      return true;
    QThread::msleep(10);
  }
  return false;
}
template <typename T>
T* widget(QWidget& parent, const char* name) {
  auto* result = parent.findChild<T*>(name);
  require(result != nullptr, name);
  return result;
}
QJsonObject profiles() {
  QJsonArray operations{
      "experiment-create",
      "experiment-baseline",
      "experiment-scan",
      "experiment-materialize",
      "experiment-reuse-selection",
      "experiment-run"};
  QJsonObject cpu{
      {"id", "cpu"},
      {"executor", "local"},
      {"capabilities", QJsonArray{"experiment-create", "experiment-reuse-selection"}},
      {"resources", QJsonObject{{"gpus", 0}}}};
  QJsonObject gpu{
      {"id", "gpu"},
      {"executor", "slurm"},
      {"capabilities", operations},
      {"runtime", QJsonObject{{"storage_namespace", "test"}}},
      {"resources", QJsonObject{{"gpus", 1}, {"gres", "gpu:rtx5090:1"}}}};
  return {{"admission_profile", "cpu"}, {"profiles", QJsonArray{cpu, gpu}}};
}
StitchingExperimentCatalog catalog(const StitchingExperimentStore& store) {
  StitchingExperimentCatalog result;
  for (int index = 1; index <= 4; ++index) {
    StoredStitchingExperiment row;
    row.workspace.root = store.directory / "sessions/session";
    row.workspace.game_id = "candidate-" + std::to_string(index);
    row.workspace.game_directory = row.workspace.root / row.workspace.game_id;
    row.workspace.invalidation_id = "generation-" + std::to_string(index);
    row.workspace.settings = {100, 3, "00:00:00", std::nullopt};
    row.sequence = index;
    row.state = "queued";
    row.source_config_revision = std::string(64, 'a');
    if (index == 1)
      row.requires_ice_mask = true;
    if (index == 2 || index == 3) {
      row.baseline_workspace_key = "sessions/session/candidate-1";
      row.selection_owner_workspace_key = "sessions/session/candidate-2";
    }
    result.experiments.push_back(row);
  }
  return result;
}
QJsonObject stage(const QJsonObject& plan, const QString& kind, const QString& candidate) {
  for (const auto& value : plan.value("stages").toArray()) {
    const auto node = value.toObject();
    if (node.value("kind") == kind && node.value("candidate_id").toString().endsWith(candidate))
      return node;
  }
  return {};
}
void plans(const StitchingExperimentStore& store) {
  auto rows = catalog(store);
  const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  auto plan = BuildStitchingWorkflowPlan(store, rows, profiles(), "gpu", 3, id);
  require(plan.ok(), "Player graph compilation failed");
  const auto base = stage(*plan, "experiment-baseline", "candidate-1");
  const auto scan = stage(*plan, "experiment-scan", "candidate-2");
  const auto materialize = stage(*plan, "experiment-materialize", "candidate-2");
  const auto sibling_selection = stage(*plan, "experiment-reuse-selection", "candidate-3");
  const auto owner_solve = stage(*plan, "experiment-run", "candidate-2");
  const auto sibling_solve = stage(*plan, "experiment-run", "candidate-3");
  const auto ordinary = stage(*plan, "experiment-run", "candidate-4");
  require(scan.value("dependencies").toArray().contains(base.value("id")), "Scan did not wait for baseline");
  require(materialize.value("dependencies").toArray().contains(scan.value("id")), "Frames did not wait for scan");
  require(
      owner_solve.value("dependencies").toArray().contains(materialize.value("id")),
      "Owner solve did not wait for materialized frames");
  require(
      sibling_selection.value("dependencies").toArray().contains(materialize.value("id")),
      "Sibling did not reuse materialized frames");
  require(
      !sibling_selection.value("dependencies").toArray().contains(owner_solve.value("id")),
      "Sibling incorrectly waits for owner matching");
  require(
      sibling_solve.value("dependencies").toArray().contains(sibling_selection.value("id")),
      "Sibling solve lacks private reuse stage");
  require(ordinary.value("dependencies").toArray().size() == 1, "Ordinary candidate acquired player dependencies");
  require(sibling_selection.value("profile_id") == "cpu", "Metadata stage requested GPU profile");
  require(
      plan->value("runtime_identity").toObject().value("user_config_snapshot").isObject(),
      "User defaults were not frozen");
  rows.experiments[0].state = "complete";
  rows.experiments[1].selection_fingerprint = "frozen";
  plan = BuildStitchingWorkflowPlan(store, rows, profiles(), "gpu", 3, id);
  require(
      plan.ok() && stage(*plan, "experiment-scan", "candidate-2").isEmpty(), "Frozen selection triggered another scan");
  rows.experiments[1].workspace.settings.manual_control_points = "manual.yaml";
  plan = BuildStitchingWorkflowPlan(store, rows, profiles(), "gpu", 3, id);
  require(
      stage(*plan, "experiment-create", "candidate-2")
              .value("inputs")
              .toObject()
              .value("workspace")
              .toObject()
              .value("settings")
              .toObject()
              .value("manual_control_points") == "manual.yaml",
      "Manual correspondences were dropped");
  rows.experiments[1].state = "frozen";
  rows.experiments[1].workflow_id = "older-workflow";
  rows.experiments[1].selection_workspace = rows.experiments[1].workspace;
  rows.experiments[1].selection_workspace->root =
      store.game_directory / "_hockeymon/workflows/older-workflow/attempts/attempt/work";
  rows.experiments[1].selection_workspace->game_directory =
      rows.experiments[1].selection_workspace->root / rows.experiments[1].selection_workspace->game_id;
  plan = BuildStitchingWorkflowPlan(store, rows, profiles(), "gpu", 3, id);
  require(
      plan.ok() && stage(*plan, "experiment-materialize", "candidate-2").isEmpty(),
      "Verified retained frames reserved another GPU materialization");
  require(
      stage(*plan, "experiment-run", "candidate-2").isEmpty(),
      "Old workflow candidate was re-admitted instead of explicitly retried");
  const auto retained = stage(*plan, "experiment-create", "candidate-2");
  require(
      retained.value("label").toString().startsWith("Prepare retained frames") &&
          !retained.value("inputs").toObject().contains("catalog_key"),
      "Retained input source acquired new catalog ownership");
  rows.experiments[1].selection_workspace.reset();
  rows.experiments[1].selection_fingerprint.clear();
  rows.experiments[1].baseline_workspace_key = "missing";
  require(!BuildStitchingWorkflowPlan(store, rows, profiles(), "gpu", 3, id).ok(), "Missing prerequisite was admitted");
}

constexpr const char* kController = R"PY(#!/usr/bin/python3
import json,os,sys,time
from pathlib import Path
root=Path(os.environ['WORKFLOW_TEST_ROOT']); args=sys.argv[1:]; operation=args[0]
def option(name): return args[args.index(name)+1]
with (root/'calls').open('a') as f: f.write(operation+'\n')
if operation=='profiles': value=json.loads((root/'profiles.json').read_text())
elif operation=='submit':
 plan=json.loads(Path(option('--plan')).read_text()); (root/'submitted.json').write_text(json.dumps(plan))
 nodes=[]
 for index,node in enumerate(plan['stages']):
  node=dict(node); node.update(state='running' if index==0 else 'waiting',reason='Waiting for verified prerequisite',result=None)
  node['attempts']=[dict(id='attempt-current',epoch=0,state='running',details={'scheduler_id':'31','host':'compute-a','reason':'Resources'})] if index==0 else []
  nodes.append(node)
 value=dict(workflow_id=plan['workflow_id'],state='running',epoch=0,revision=1,nodes=nodes)
 (root/'state.json').write_text(json.dumps(value))
 if (root/'lose-submit-response').exists():
  (root/'status-unavailable').write_text('2'); print('Submission response unavailable',file=sys.stderr); raise SystemExit(1)
elif operation=='export':
 plan=json.loads(Path(option('--plan')).read_text()); destination=Path(option('--output')); destination.write_text('#!/bin/sh\n'); destination.with_suffix('.json').write_text(json.dumps(plan)); value={'script':str(destination),'workflow_id':plan['workflow_id']}
elif operation=='status':
 time.sleep(.03)
 unavailable=root/'status-unavailable'
 if unavailable.exists() and int(unavailable.read_text())>0:
  unavailable.write_text(str(int(unavailable.read_text())-1)); print('Workflow registration temporarily unavailable',file=sys.stderr); raise SystemExit(1)
 value=json.loads((root/'state.json').read_text())
elif operation in ('cancel','retry'):
 value=json.loads((root/'state.json').read_text()); assert int(option('--epoch'))==value['epoch']
 if operation=='cancel': value['state']='cancelled'; value['cancel_requested']=True
 else:
  value['epoch']+=1; value['state']='running'
  node=next(n for n in value['nodes'] if n['id']==option('--stage')); node['state']='running'
  node['attempts'].append(dict(id='attempt-retry',epoch=value['epoch'],state='running',details={}))
 (root/'state.json').write_text(json.dumps(value))
elif operation=='log': value=dict(text=option('--stage')+' '+option('--attempt')+' retained log')
else: raise SystemExit(9)
print(json.dumps(value))
)PY";
void ui(const QString& root, const StitchingExperimentStore& store, YAML::Node baseline) {
  QDir().mkpath(root + "/scripts");
  write(root + "/scripts/hstream-workflow", kController);
  QFile::setPermissions(
      root + "/scripts/hstream-workflow", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  write(root + "/profiles.json", QJsonDocument(profiles()).toJson());
  baseline["workflow"]["registry"] = root.toStdString();
  baseline["workflow"]["poll_interval_ms"] = 100;
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("WORKFLOW_TEST_ROOT", root);
  QWidget shell;
  auto* layout = new QVBoxLayout(&shell);
  auto* outer = new QScrollArea(&shell);
  outer->setWidgetResizable(true);
  layout->addWidget(outer);
  auto* content = new QWidget(outer);
  auto* content_layout = new QVBoxLayout(content);
  content_layout->setAlignment(Qt::AlignTop);
  outer->setWidget(content);
  auto* pane = new StitchingWorkflow(
      QString::fromStdString(store.game_directory.string()), root, environment, baseline, content);
  content_layout->addWidget(pane);
  content->setMinimumHeight(900);
  pane->setEditingEnabled(true, true);
  shell.resize(700, 650);
  shell.show();
  auto* profiles_control = widget<QComboBox>(*pane, "stitchWorkflowProfile");
  require(wait([&] { return profiles_control->count() == 2; }), "Profiles were not loaded asynchronously");
  require(!pane->selected(), "Managed execution silently replaced Current runner");
  profiles_control->setCurrentIndex(1);
  auto* parallel = widget<QSpinBox>(*pane, "stitchWorkflowParallel");
  parallel->setValue(3);
  int ticks = 0;
  QTimer responsive;
  QObject::connect(&responsive, &QTimer::timeout, &shell, [&] { ++ticks; });
  responsive.start(10);
  const auto old_profile = profiles_control->currentIndex();
  outer->verticalScrollBar()->setValue(0);
  QWheelEvent wheel(
      QPointF(4, 4),
      QPointF(profiles_control->mapToGlobal(QPoint(4, 4))),
      QPoint(),
      QPoint(0, -120),
      Qt::NoButton,
      Qt::NoModifier,
      Qt::NoScrollPhase,
      false);
  QApplication::sendEvent(profiles_control, &wheel);
  require(
      profiles_control->currentIndex() == old_profile && outer->verticalScrollBar()->value() > 0,
      "Workflow profile wheel changed value or failed to scroll pane");
  outer->verticalScrollBar()->setValue(0);
  bool exported = false;
  QString export_message;
  pane->message = [&](const QString& message, bool error) {
    if (!error && message.startsWith("Exported batch:")) {
      exported = true;
      export_message = message;
    }
  };
  const auto exported_path = root + "/exported-batch.sh";
  QTimer::singleShot(20, pane, [exported_path] {
    for (auto* top : QApplication::topLevelWidgets())
      if (auto* dialog = qobject_cast<QFileDialog*>(top)) {
        dialog->setDirectory(QFileInfo(exported_path).absolutePath());
        dialog->selectFile(QFileInfo(exported_path).fileName());
        QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
      }
  });
  pane->exportBatch(store, catalog(store));
  require(wait([&] { return exported; }), "Export did not return an executable batch destination");
  require(!QFileInfo::exists(root + "/submitted.json"), "Export unexpectedly submitted compute");
  require(
      QFileInfo::exists(export_message.mid(QString("Exported batch: ").size())),
      "Export response did not identify the created file");
  write(root + "/lose-submit-response", "lost");
  pane->start(store, catalog(store));
  require(
      wait([&] { return QFileInfo::exists(root + "/submitted.json") && pane->hasSelection(); }),
      "Accepted workflow with lost submit response and unavailable first status was not polled to recovery");
  require(
      read(root + "/state.json").value("workflow_id") == pane->workflowId(),
      "Submission recovery changed the intended workflow identity");
  require(pane->active() && ticks > 1, "Workflow request blocked Qt event processing");
  const auto submitted = read(root + "/submitted.json");
  require(
      submitted.value("limits").toObject().value("max_parallel").toInt() == 3,
      "Concurrency selection was not submitted");
  auto* stages = widget<QComboBox>(*pane, "stitchWorkflowStage");
  auto* attempts = widget<QComboBox>(*pane, "stitchWorkflowAttempt");
  require(stages->isEnabled() && stages->count() > 4, "Running stage selection is disabled");
  pane->showLog();
  require(
      wait([&] {
        auto* log = pane->findChild<QPlainTextEdit*>("stitchWorkflowLog");
        return log && log->toPlainText().contains("attempt-current retained log");
      }),
      "Running attempt log was not available");
  QJsonObject state = read(root + "/state.json");
  state.insert("state", "failed");
  auto nodes = state.value("nodes").toArray();
  auto node = nodes[0].toObject();
  node.insert("state", "failed");
  auto history = node.value("attempts").toArray();
  auto old = history[0].toObject();
  old.insert("state", "failed");
  history[0] = old;
  node.insert("attempts", history);
  nodes[0] = node;
  state.insert("nodes", nodes);
  write(root + "/state.json", QJsonDocument(state).toJson());
  auto* retry = widget<QPushButton>(*pane, "retryStitchWorkflowStageButton");
  state.insert("owner_released", false);
  write(root + "/state.json", QJsonDocument(state).toJson());
  require(
      wait([&] {
        return widget<QPlainTextEdit>(*pane, "stitchWorkflowDetails")->toPlainText().contains("State: failed");
      }),
      "Terminal stage was not observed");
  require(pane->active() && !retry->isEnabled(), "Terminal stage bypassed pending ownership release");
  state.insert("owner_released", true);
  write(root + "/state.json", QJsonDocument(state).toJson());
  require(wait([&] { return retry->isEnabled(); }), "Failed selected stage cannot retry");
  for (auto* button : pane->findChildren<QPushButton*>())
    require(!button->icon().isNull(), "Workflow action has no icon");
  retry->click();
  require(
      wait([&] { return attempts->count() == 2 && pane->active(); }),
      "Retry did not retain history and start a new epoch");
  attempts->setCurrentIndex(0);
  require(
      wait([&] {
        return widget<QPlainTextEdit>(*pane, "stitchWorkflowLog")
            ->toPlainText()
            .contains("attempt-current retained log");
      }),
      "Old attempt log selection was lost");
  const auto id = pane->workflowId();
  const auto screenshot = qEnvironmentVariable("HSTREAM_WORKFLOW_SCREENSHOT");
  if (!screenshot.isEmpty())
    require(shell.grab().save(screenshot), "Could not capture workflow pane screenshot");
  delete pane;
  QApplication::processEvents();
  QFile calls(root + "/calls");
  require(calls.open(QIODevice::ReadOnly), "Cannot read calls");
  require(!calls.readAll().contains("cancel"), "Closing workflow pane cancelled compute");
  calls.close();
  pane = new StitchingWorkflow(
      QString::fromStdString(store.game_directory.string()), root, environment, baseline, content);
  content_layout->addWidget(pane);
  pane->attach(id);
  require(wait([&] { return pane->hasSelection(); }), "Reopening did not restore workflow status");
  pane->cancel();
  require(
      wait([&] { return !pane->active() && !pane->busy(); }), "Explicit cancellation did not use the current epoch");
  delete pane;
}
void dialogOwnership(const QString& root) {
  const auto game = root + "/dialog-game";
  QDir().mkpath(game + "/_hockeymon");
  write(
      game + "/_hockeymon/config.yaml",
      ("game:\n  videos:\n    left: [left.mp4]\n    right: [right.mp4]\nworkflow:\n  registry: " + root +
       "\n  poll_interval_ms: 100\n")
          .toUtf8());
  write(game + "/left.mp4", "left");
  write(game + "/right.mp4", "right");
  const auto runner = root + "/runner.sh";
  write(runner, "#!/bin/sh\nsleep 30\n");
  QFile::setPermissions(runner, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("WORKFLOW_TEST_ROOT", root);
  StitchingExperimentDialog dialog(
      game, runner, root, game + "/_hockeymon/config.yaml", environment, 100, 1, "00:00:00");
  dialog.show();
  auto* rows = widget<QTableWidget>(dialog, "stitchExperimentCandidates");
  auto* add = widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton");
  auto* start = widget<QPushButton>(dialog, "startStitchExperimentBatchButton");
  auto* cancel = widget<QPushButton>(dialog, "cancelStitchExperimentsButton");
  auto main = hm::game_operation::LocalOwner("pipeline", "test-main", "test-main-token");
  {
    auto lock = hm::game_operation::Lock::Acquire(game.toStdString());
    require(lock.ok(), "Cannot lock fixture game");
    require((*lock)->Claim(main).ok(), "Cannot claim main operation");
  }
  add->click();
  require(rows->rowCount() == 0, "Main-owned game acquired queued experiment rows");
  {
    auto lock = hm::game_operation::Lock::Acquire(game.toStdString());
    require((*lock)->Release(main.token, main.revision, true).ok(), "Cannot release fixture main owner");
  }
  add->click();
  require(wait([&] { return start->isEnabled(); }), "Preparing candidate did not complete");
  start->click();
  require(
      wait([&] {
        auto value = hm::game_operation::Inspect(game.toStdString());
        return cancel->isEnabled() && value.ok() && value->has_value() && (**value).kind == "experiment";
      }),
      "Local batch has no durable game ownership");
  {
    auto lock = hm::game_operation::Lock::Acquire(game.toStdString());
    require(!(*lock)->Claim(main).ok(), "Main playback bypassed active experiment ownership");
  }
  cancel->click();
  require(
      wait(
          [&] {
            auto value = hm::game_operation::Inspect(game.toStdString());
            return !cancel->isEnabled() && value.ok() && !value->has_value();
          },
          10000),
      "Cancelled batch released controls before descendant cleanup or never retired owner");
  widget<QLineEdit>(dialog, "stitchExperimentFrameCounts")->setText("3");
  widget<QLineEdit>(dialog, "stitchExperimentControlPoints")->setText("100,150");
  widget<QCheckBox>(dialog, "stitchExperimentPreferPlayerFrames")->setChecked(true);
  add->click();
  require(wait([&] { return start->isEnabled(); }), "Player workflow candidates were not prepared");
  auto* profile = widget<QComboBox>(dialog, "stitchWorkflowProfile");
  require(wait([&] { return profile->count() == 2; }), "Dialog did not load execution profiles");
  profile->setCurrentIndex(1);
  start->click();
  require(
      wait([&] { return widget<QComboBox>(dialog, "stitchWorkflowStage")->count() > 4 && cancel->isEnabled(); }),
      "Dialog did not submit a dependency graph");
  require(
      widget<QComboBox>(dialog, "stitchWorkflowStage")->currentText().startsWith("Solve"),
      "Selected row did not select its solve stage");
  require(
      rows->isEnabled() && widget<QPushButton>(dialog, "viewStitchExperimentRunnerLogButton")->isEnabled(),
      "Running workflow blocked stage inspection");
  const auto screenshot = qEnvironmentVariable("HSTREAM_WORKFLOW_DIALOG_SCREENSHOT");
  if (!screenshot.isEmpty())
    require(dialog.grab().save(screenshot), "Could not capture distributed experiment dialog");
  QFile before(root + "/calls");
  require(before.open(QIODevice::ReadOnly), "Cannot read controller calls");
  const auto calls = before.readAll();
  before.close();
  dialog.close();
  require(wait([&] { return !dialog.isVisible(); }), "Closing submitted workflow did not detach");
  QFile after(root + "/calls");
  require(after.open(QIODevice::ReadOnly), "Cannot reread controller calls");
  require(after.readAll().count("cancel") == calls.count("cancel"), "Dialog close cancelled submitted jobs");
  // Real submission owns the game durably. Reopening must use advisory config
  // reads, not attempt transaction recovery through the mutation admission gate.
  auto store = OpenStitchingExperimentStore(game.toStdString());
  require(store.ok(), "Cannot reopen workflow ownership store");
  auto saved = LoadStitchingExperimentStore(*store);
  require(saved.ok(), "Cannot read workflow ownership fixture rows");
  const auto id = read(root + "/state.json").value("workflow_id").toString().toStdString();
  auto remote = hm::game_operation::LocalOwner("workflow", id, "remote-workflow-token");
  remote.workflow_directory = (store->game_directory / "_hockeymon/workflows" / id).string();
  {
    auto lock = hm::game_operation::Lock::Acquire(store->game_directory);
    require(lock.ok() && (*lock)->Claim(remote).ok(), "Cannot bind remote ownership fixture");
  }
  hm::game_operation::RegisterAuthorization(store->game_directory, remote.token);
  for (auto& row : saved->experiments) {
    row.workflow_id = id;
    row.workflow_directory = remote.workflow_directory;
    row.workflow_active = true;
    require(SaveStitchingExperiment(*store, row).ok(), "Cannot bind remote catalog fixture");
  }
  hm::game_operation::ForgetAuthorization(store->game_directory, remote.token);
  QFile main_config(game + "/_hockeymon/config.yaml");
  require(main_config.open(QIODevice::ReadOnly), "Cannot snapshot main config");
  const auto before_reopen = main_config.readAll();
  main_config.close();
  {
    StitchingExperimentDialog reopened(
        game, runner, root, game + "/_hockeymon/config.yaml", environment, 100, 1, "00:00:00");
    reopened.show();
    require(
        wait([&] {
          return widget<QComboBox>(reopened, "stitchWorkflowStage")->count() > 4 &&
              widget<QPushButton>(reopened, "cancelStitchExperimentsButton")->isEnabled();
        }),
        "Workflow-owned game did not reopen and attach to running stages");
    const auto owner = hm::game_operation::Inspect(store->game_directory);
    require(
        owner.ok() && owner->has_value() && (**owner).id == id && (**owner).revision == remote.revision,
        "Read-only reopen changed durable workflow admission");
    require(
        main_config.open(QIODevice::ReadOnly) && main_config.readAll() == before_reopen,
        "Read-only workflow reopen rewrote the main configuration");
    reopened.close();
    require(wait([&] { return !reopened.isVisible(); }), "Reopened monitor did not detach");
  }
  {
    auto lock = hm::game_operation::Lock::Acquire(store->game_directory);
    require(lock.ok() && (*lock)->Release(remote.token, remote.revision, true).ok(), "Cannot retire remote test owner");
  }
}

} // namespace
int main(int argc, char** argv) {
  QApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
  QApplication app(argc, argv);
  install_value_control_wheel_guard();
  try {
    QTemporaryDir temporary;
    require(temporary.isValid(), "No fixture directory");
    qputenv("HOME", temporary.path().toUtf8());
    qputenv("HSTREAM_JOB_MANAGED_RUNTIME", "1");
    auto baseline = hm::baseline_config::load();
    require(baseline.ok(), "Cannot load baseline");
    StitchingExperimentStore store;
    store.game_directory = temporary.path().toStdString() + "/game";
    store.directory = store.game_directory / "_hockeymon/stitching-experiments";
    plans(store);
    ui(temporary.path(), store, YAML::Clone(baseline->values));
    dialogOwnership(temporary.path());
    std::cout << "Workflow DAG, async UI, retry/log history, wheel routing, detach/reopen and cancellation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
