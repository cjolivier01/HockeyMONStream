#include "src/apps/hstream-ui/StitchingWorkflow.h"
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/GameOperationOwner.h"
#include "src/apps/hstream-ui/StitchingExperimentDialog.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtCore/QElapsedTimer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtGui/QKeyEvent>
#include <QtGui/QMouseEvent>
#include <QtGui/QRegion>
#include <QtGui/QWheelEvent>
#include <QtGui/QWindow>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QMainWindow>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QScrollBar>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QTabBar>
#include <QtWidgets/QTabWidget>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>

#include <unistd.h>
#include <csignal>
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
      {"label", "RTX4090 / RTX5090"},
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
  require(base.value("label") == "Initial calibration · #1", "Initial calibration label is unclear");
  require(scan.value("label") == "Find player-rich frames · #2", "Player search label is unclear");
  require(materialize.value("label") == "Extract selected frames · #2", "Frame extraction label is unclear");
  require(owner_solve.value("label") == "Calibrate · 100 CP · #2", "Calibration omitted its control-point limit");
  require(sibling_selection.value("label") == "Use shared frames (CPU) · #3", "Shared frame copy label is unclear");
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
  rows.experiments[3].saved_selection_fingerprint = std::string(64, 'b');
  rows.experiments[3].selection_fingerprint = rows.experiments[3].saved_selection_fingerprint;
  const auto retained_queue = BuildStitchingWorkflowPlan(store, rows, profiles(), "gpu", 3, id);
  require(
      retained_queue.ok() &&
          stage(*retained_queue, "experiment-create", "candidate-4")
              .value("label")
              .toString()
              .startsWith("Set up retained frames (CPU)") &&
          retained_queue->value("stages").toArray().size() == plan->value("stages").toArray().size() &&
          stage(*retained_queue, "experiment-run", "candidate-4") == ordinary &&
          stage(*retained_queue, "experiment-scan", "candidate-4").isEmpty(),
      "Queued saved frames must be labeled retained without changing the solve plan or adding a scan");
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
      retained.value("label").toString().startsWith("Set up retained frames (CPU)") &&
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
 plan=json.loads(Path(option('--plan')).read_text())
 while (root/'hold-submit').exists(): time.sleep(.01)
 if (root/'reject-submit').exists():
  print(json.dumps(dict(error='Resource profile changed before admission',submission='rejected',workflow_id=plan['workflow_id'])),file=sys.stderr); raise SystemExit(2)
 (root/'submitted.json').write_text(json.dumps(plan))
 nodes=[]
 for index,node in enumerate(plan['stages']):
  node=dict(node); node.update(state='running' if index==0 else 'waiting',reason='Waiting for verified prerequisite',result=None)
  node['attempts']=[dict(id='attempt-current',epoch=0,state='running',details={'scheduler_id':'31','host':'compute-a','reason':'Resources'})] if index==0 else []
  nodes.append(node)
 value=dict(workflow_id=plan['workflow_id'],state='running',owner_released=False,epoch=0,revision=1,nodes=nodes,limits=plan['limits'])
 (root/'state.json').write_text(json.dumps(value))
 if (root/'lose-submit-response').exists():
  if not (root/'skip-submit-status-failures').exists(): (root/'status-unavailable').write_text('2')
  print('Submission response unavailable',file=sys.stderr); raise SystemExit(1)
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
 if operation=='cancel':
  if (root/'reject-cancel').exists():
   print('Cancellation rejected by fixture controller',file=sys.stderr); raise SystemExit(1)
  value['state']='cancelled'; value['cancel_requested']=True; value['owner_released']=not (root/'hold-owner-release').exists()
  for node in value['nodes']: node['state']='cancelled'
 else:
  value['epoch']+=1; value['state']='running'
  node=next(n for n in value['nodes'] if n['id']==option('--stage')); node['state']='running'
  node['attempts'].append(dict(id='attempt-retry',epoch=value['epoch'],state='running',details={}))
 (root/'state.json').write_text(json.dumps(value))
 if operation=='cancel' and (root/'lose-cancel-response').exists():
  print('Cancellation response unavailable',file=sys.stderr); raise SystemExit(1)
elif operation=='log': value=dict(text=option('--stage')+' '+option('--attempt')+' retained log')
else: raise SystemExit(9)
print(json.dumps(value))
)PY";
void profileResponseBounds(const QString& root, YAML::Node baseline) {
  baseline["workflow"]["registry"] = root.toStdString();
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("WORKFLOW_TEST_ROOT", root);
  for (const int megabytes : {6, 9}) {
    auto catalog = profiles();
    catalog.insert("fixture_padding", QString(megabytes * 1024 * 1024, 'x'));
    write(root + "/profiles.json", QJsonDocument(catalog).toJson(QJsonDocument::Compact));
    StitchingWorkflow pane(root + "/game", root, environment, baseline, nullptr);
    bool rejected = false;
    pane.message = [&](const QString&, bool error) { rejected = error; };
    auto* selector = widget<QComboBox>(pane, "stitchWorkflowProfile");
    if (megabytes < 8) {
      require(wait([&] { return selector->count() == 2 || rejected; }), "Large profile catalog did not finish loading");
      require(!rejected && selector->count() == 2, "Valid profile catalog between 4 and 8 MiB was rejected");
    } else {
      require(wait([&] { return rejected; }), "Profile catalog above 8 MiB was not rejected");
      require(selector->count() == 1 && !selector->isEnabled(), "Oversized profile catalog enabled managed execution");
    }
  }
  write(root + "/profiles.json", QJsonDocument(profiles()).toJson());
}

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
  require(profiles_control->currentText() == "RTX4090 / RTX5090 (slurm)", "Profile ignored its readable label");
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
  // The oversized container above exists only to exercise wheel routing.
  // Return to the available viewport for monitoring and resize checks.
  content->setMinimumHeight(0);
  content_layout->setAlignment(Qt::Alignment{});
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
  write(root + "/reject-submit", "rejected");
  pane->start(store, catalog(store));
  require(
      wait([&] { return !pane->busy() && !pane->active(); }),
      "Definite submission rejection retained active ownership");
  require(pane->workflowId().isEmpty() && profiles_control->isEnabled(), "Rejected submission did not restore editing");
  require(!QFileInfo::exists(root + "/submitted.json"), "Rejected submission created compute intent");
  QFile::remove(root + "/reject-submit");
  require(
      QFile::rename(root + "/scripts/hstream-workflow", root + "/scripts/controller-away"),
      "Cannot hide fixture launcher");
  pane->start(store, catalog(store));
  require(
      wait([&] { return !pane->busy() && !pane->active(); }), "Failed-to-start controller retained active ownership");
  require(pane->workflowId().isEmpty(), "Failed-to-start controller retained workflow identity");
  require(
      QFile::rename(root + "/scripts/controller-away", root + "/scripts/hstream-workflow"),
      "Cannot restore fixture launcher");
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
  auto* graph_scroll = widget<QScrollArea>(*pane, "stitchWorkflowGraphScroll");
  auto* monitor_scroll = widget<QScrollArea>(*pane, "stitchWorkflowMonitorScroll");
  const auto graph_nodes = submitted.value("stages").toArray();
  auto* graph = widget<QWidget>(*pane, "stitchWorkflowGraph");
  const auto check_graph = [&] {
    require(graph_scroll->horizontalScrollBar()->maximum() == 0, "Workflow still requires horizontal traversal");
    const auto buttons = graph->findChildren<QPushButton*>();
    require(buttons.size() == graph_nodes.size(), "Responsive graph lost a diagnostic stage");
    for (const auto& value : graph_nodes) {
      const auto node = value.toObject();
      auto* button = widget<QPushButton>(*graph, qPrintable("stitchWorkflowNode_" + node.value("id").toString()));
      require(
          button->geometry().left() >= 0 && button->geometry().right() < graph->width(),
          "Workflow stage extends beyond the pane");
      require(
          button->toolTip().contains("Prerequisites:") && button->toolTip().contains("Used by:"),
          "Workflow stage tooltip lost dependency explanations");
      for (const auto& dependency : node.value("dependencies").toArray()) {
        auto* parent = widget<QPushButton>(*graph, qPrintable("stitchWorkflowNode_" + dependency.toString()));
        require(parent->geometry().bottom() < button->geometry().top(), "Dependency no longer flows top to bottom");
      }
      for (auto* peer : buttons)
        require(peer == button || !peer->geometry().intersects(button->geometry()), "Responsive stages overlap");
    }
  };
  require(wait([&] { return graph_scroll->horizontalScrollBar()->maximum() == 0; }), "Graph did not fit its pane");
  check_graph();
  const auto original_state = read(root + "/state.json");
  const auto original_palette = QApplication::palette();
  const auto first_id = graph_nodes[0].toObject().value("id").toString();
  const auto first_button = [&] { return widget<QPushButton>(*graph, qPrintable("stitchWorkflowNode_" + first_id)); };
  for (const bool dark : {false, true}) {
    auto colors = original_palette;
    colors.setColor(QPalette::Button, dark ? QColor("#242a32") : QColor("#eeeeee"));
    colors.setColor(QPalette::ButtonText, dark ? Qt::white : Qt::black);
    QApplication::setPalette(colors);
    for (const auto& [status, border] : std::vector<std::pair<QString, QColor>>{
             {"running", QColor("#68b8f0")},
             {"succeeded", QColor("#71ba8c")},
             {"reused", QColor("#71ba8c")},
             {"failed", QColor("#ec939b")},
             {"recovery", QColor("#ec939b")},
             {"waiting", colors.color(QPalette::Mid)}}) {
      auto snapshot = original_state;
      auto entries = snapshot.value("nodes").toArray();
      auto entry = entries[0].toObject();
      entry.insert("state", status);
      entries[0] = entry;
      snapshot.insert("nodes", entries);
      write(root + "/state.json", QJsonDocument(snapshot).toJson());
      require(
          wait([&] { return first_button()->accessibleName().endsWith(" · " + status); }),
          "Graph did not announce the updated stage state");
      for (const bool selected : {false, true}) {
        stages->setCurrentIndex(selected ? 0 : 1);
        auto* button = first_button();
        const auto image = button->grab().toImage();
        require(
            image.pixelColor(12, 7) == colors.color(QPalette::Button) &&
                button->palette().color(QPalette::ButtonText) == colors.color(QPalette::ButtonText),
            "State icon must preserve the theme's neutral node fill and readable text");
        require(button->isChecked() == selected, "Status icon lost the independent selection marker");
        require(!button->icon().isNull() && button->text().endsWith(status), "Status icon hid accessible state text");
        if (status != "waiting") {
          const QColor expected_border = selected ? QColor("#ffd166") : border;
          bool marked = false;
          for (int x = 6; x < image.width() - 6; ++x)
            marked |= image.pixelColor(x, 1) == expected_border;
          require(marked, "Status icon changed the existing state border or selection marker");
          require(button->iconSize() == QSize(20, 20), "Workflow status glyph must be readable at 20 pixels");
          const auto glyph = button->icon().pixmap(20, 20).toImage();
          const QColor expected_color(status == "failed" || status == "recovery" ? "#d64545" : "#229954");
          bool colored = false;
          for (int y = 0; y < glyph.height(); ++y)
            for (int x = 0; x < glyph.width(); ++x) {
              const auto pixel = glyph.pixelColor(x, y);
              colored |= pixel.alpha() > 200 && pixel.red() == expected_color.red() &&
                  pixel.green() == expected_color.green() && pixel.blue() == expected_color.blue();
            }
          require(colored, "Workflow status glyph lost its green completion/running or red failure color");
        }
        const auto screenshots = qEnvironmentVariable("HSTREAM_WORKFLOW_STATUS_SCREENSHOT_DIR");
        if (selected && !screenshots.isEmpty())
          require(
              shell.grab().save(screenshots + "/" + (dark ? "dark-" : "light-") + status + ".png"),
              "Could not capture themed workflow state");
      }
    }
  }
  QApplication::setPalette(original_palette);
  write(root + "/state.json", QJsonDocument(original_state).toJson());
  require(
      wait([&] { return first_button()->accessibleName().endsWith(" · running"); }), "Running state was not restored");
  stages->setCurrentIndex(0);
  QApplication::processEvents();
  graph_scroll->verticalScrollBar()->setValue(std::min(7, graph_scroll->verticalScrollBar()->maximum()));
  const auto selected_stage = stages->currentData();
  const auto selected_attempt = attempts->currentData();
  const int scroll_position = graph_scroll->verticalScrollBar()->value();
  const QSize graph_size = graph->size();
  std::vector<std::pair<QPointer<QPushButton>, QRect>> node_geometry;
  for (auto* button : graph->findChildren<QPushButton*>())
    node_geometry.emplace_back(button, button->geometry());
  const auto unchanged_graph = [&] {
    require(
        graph->size() == graph_size && graph_scroll->verticalScrollBar()->value() == scroll_position &&
            stages->currentData() == selected_stage && attempts->currentData() == selected_attempt,
        "Running animation changed graph dimensions, scrolling or diagnostic selection");
    for (const auto& [button, geometry] : node_geometry)
      require(button && button->geometry() == geometry, "Running animation rebuilt or moved a workflow node");
  };
  const auto observe_icon = [&](bool animated) {
    const auto initial = first_button()->icon().pixmap(20, 20).toImage();
    bool changed = false;
    QElapsedTimer elapsed;
    elapsed.start();
    require(
        wait([&] {
          unchanged_graph();
          changed |= first_button()->icon().pixmap(20, 20).toImage() != initial;
          return elapsed.elapsed() >= 400;
        }),
        "Workflow icon observation stalled");
    require(changed == animated, "Running status must animate only while its graph is visible");
  };
  auto* animation = widget<QTimer>(*graph, "stitchWorkflowAnimation");
  require(animation->isActive(), "Visible running workflow did not start its animation timer");
  observe_icon(true);
  graph->hide();
  QApplication::processEvents();
  require(!animation->isActive(), "Hidden workflow graph kept its animation timer active");
  observe_icon(false);
  graph->show();
  QApplication::processEvents();
  require(animation->isActive(), "Showing a running workflow did not resume its animation timer");
  observe_icon(true);
  graph_scroll->verticalScrollBar()->setValue(0);
  for (const QString status : {"succeeded", "failed"}) {
    auto snapshot = original_state;
    auto entries = snapshot.value("nodes").toArray();
    auto entry = entries[0].toObject();
    entry.insert("state", status);
    entries[0] = entry;
    snapshot.insert("nodes", entries);
    write(root + "/state.json", QJsonDocument(snapshot).toJson());
    require(
        wait([&] { return first_button()->accessibleName().endsWith(" · " + status); }),
        "Terminal workflow icon did not arrive");
    require(!animation->isActive(), "Workflow with only terminal or waiting stages kept animating");
    const auto terminal_icon = first_button()->icon().pixmap(20, 20).toImage();
    QElapsedTimer elapsed;
    elapsed.start();
    require(
        wait([&] {
          require(
              first_button()->icon().pixmap(20, 20).toImage() == terminal_icon,
              "Completed or failed workflow node kept animating");
          return elapsed.elapsed() >= 400;
        }),
        "Terminal workflow icon observation stalled");
  }
  write(root + "/state.json", QJsonDocument(original_state).toJson());
  require(
      wait([&] { return first_button()->accessibleName().endsWith(" · running"); }), "Running state was not restored");
  const auto shared = stage(submitted, "experiment-materialize", "candidate-2");
  auto* shared_button = widget<QPushButton>(*graph, qPrintable("stitchWorkflowNode_" + shared.value("id").toString()));
  shared_button->click();
  require(
      stages->currentData().toString() == shared.value("id").toString(),
      "Clicking shared frames did not select its diagnostic stage");
  const auto description = widget<QPlainTextEdit>(*pane, "stitchWorkflowDetails")->toPlainText();
  require(
      description.contains("Find player-rich frames") && description.contains("Use shared frames (CPU)") &&
          description.contains("Calibrate · 100 CP"),
      "Shared parent details did not explain its fan-out");
  auto legacy = read(root + "/state.json");
  auto legacy_nodes = legacy.value("nodes").toArray();
  for (int index = 0; index < legacy_nodes.size(); ++index) {
    auto node = legacy_nodes[index].toObject();
    if (node.value("id") == shared.value("id")) {
      node.insert("label", "Materialize · 27");
      legacy_nodes[index] = node;
    }
  }
  legacy.insert("nodes", legacy_nodes);
  write(root + "/state.json", QJsonDocument(legacy).toJson());
  require(
      wait([&] { return stages->currentText().startsWith("Extract selected frames · #27"); }),
      "A sealed legacy plan still displays internal stage jargon");
  require(read(root + "/state.json") == legacy, "Display relabeling modified the sealed snapshot");
  auto* expand = widget<QPushButton>(*pane, "expandStitchWorkflowButton");
  expand->click();
  auto* expanded = widget<QDialog>(*pane, "stitchWorkflowExpandedWindow");
  require(
      wait([&] { return expanded->isVisible() && graph_scroll->viewport()->width() > 800; }),
      "Expand workflow did not provide a wide live monitor");
  require(
      expanded->windowFlags().testFlag(Qt::WindowMaximizeButtonHint) && expanded->isSizeGripEnabled(),
      "Expanded workflow cannot maximize or resize");
  check_graph();
  require(animation->isActive(), "Expanded workflow lost its running animation");
  require(stages->currentData().toString() == shared.value("id").toString(), "Expanding workflow lost stage selection");
  const int expanded_height = graph_scroll->viewport()->height();
  expanded->resize(expanded->width(), expanded->height() + 150);
  require(
      wait([&] { return graph_scroll->viewport()->height() >= expanded_height + 100; }),
      "Expanded workflow did not give extra height to the graph");
  const auto expanded_screenshot = qEnvironmentVariable("HSTREAM_WORKFLOW_EXPANDED_SCREENSHOT");
  if (!expanded_screenshot.isEmpty())
    require(expanded->grab().save(expanded_screenshot), "Could not capture expanded workflow");
  expand->click();
  require(
      wait([&] { return !pane->findChild<QDialog*>("stitchWorkflowExpandedWindow"); }),
      "Restore workflow left its detached window open");
  require(
      stages->currentData().toString() == shared.value("id").toString() && graph_scroll->window() == &shell,
      "Restore workflow lost selection or detached the monitor");
  expand->click();
  widget<QDialog>(*pane, "stitchWorkflowExpandedWindow")->close();
  require(wait([&] { return graph_scroll->window() == &shell; }), "Closing expanded workflow did not restore it");
  stages->setCurrentIndex(0);
  require(
      wait([&] { return graph_scroll->viewport()->height() > 150; }), "Workflow graph left spare pane space unused");
  const int graph_height = graph_scroll->viewport()->height();
  shell.resize(700, 850);
  require(
      wait([&] { return graph_scroll->viewport()->height() >= graph_height + 150; }),
      "Growing the workflow pane did not grow the graph viewport");
  shell.resize(460, 350);
  require(wait([&] { return graph_scroll->viewport()->width() < 450; }), "Graph did not shrink with the pane");
  check_graph();
  auto* retry_control = widget<QPushButton>(*pane, "retryStitchWorkflowStageButton");
  require(
      wait([&] { return monitor_scroll->verticalScrollBar()->maximum() > 0; }), "Small monitor cannot scroll controls");
  monitor_scroll->ensureWidgetVisible(retry_control);
  require(
      wait([&] {
        return monitor_scroll->viewport()->rect().contains(
            QRect(retry_control->mapTo(monitor_scroll->viewport(), QPoint()), retry_control->size()));
      }),
      "Small workflow pane left its retry control unreachable");
  shell.resize(700, 650);
  monitor_scroll->verticalScrollBar()->setValue(0);
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
  require(
      wait([&] {
        return widget<QComboBox>(*pane, "stitchWorkflowProfile")->currentData().toString() == "gpu" &&
            widget<QSpinBox>(*pane, "stitchWorkflowParallel")->value() == 3;
      }),
      "Reopening did not restore the frozen profile and parallel limit");
  pane->cancel();
  require(
      wait([&] { return !pane->active() && !pane->busy(); }), "Explicit cancellation did not use the current epoch");
  delete pane;
}
int controllerCalls(const QString& root, const QByteArray& operation) {
  QFile calls(root + "/calls");
  require(calls.open(QIODevice::ReadOnly), "Cannot inspect controller calls");
  return calls.readAll().count(operation + "\n");
}

void closeWorkflow(StitchingExperimentDialog& dialog, const QString& route, bool cancel, bool expect_prompt = true) {
  bool prompted = false;
  bool safe_default = false;
  QTimer::singleShot(0, &dialog, [&] {
    if (auto* prompt = dialog.findChild<QMessageBox*>("stitchExperimentWorkflowCloseGuard")) {
      prompted = true;
      auto* stop = prompt->findChild<QPushButton*>("stitchExperimentCloseWorkflowCancel");
      auto* keep = prompt->findChild<QPushButton*>("stitchExperimentCloseWorkflowKeep");
      safe_default = stop && keep && prompt->defaultButton() == keep && prompt->escapeButton() == keep &&
          !stop->icon().isNull() && !keep->icon().isNull();
      const auto screenshots = qEnvironmentVariable("HSTREAM_WORKFLOW_CLOSE_SCREENSHOT_DIR");
      if (!screenshots.isEmpty())
        prompt->grab().save(screenshots + "/close-confirmation.png");
      if (safe_default)
        (cancel ? stop : keep)->click();
      else
        prompt->reject();
    }
  });
  if (route == "button")
    widget<QPushButton>(dialog, "closeStitchExperimentButton")->click();
  else if (route == "window")
    dialog.windowHandle()->close();
  else {
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&dialog, &escape);
  }
  QApplication::processEvents();
  require(
      expect_prompt ? prompted && safe_default : !prompted,
      "Workflow Close must confirm cancellation once with Keep as default, then retain the pending cleanup");
}

void waitForDeleted(QPointer<StitchingExperimentDialog>& dialog) {
  if (!dialog)
    return;
  // A real loop delivers DeferredDelete as HStreamWindow's show() does; a
  // processEvents-only polling loop cannot prove WA_DeleteOnClose behavior.
  QEventLoop closed;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, &closed, &QEventLoop::quit);
  QObject::connect(dialog, &QObject::destroyed, &closed, &QEventLoop::quit);
  timeout.start(5000);
  closed.exec();
  require(
      dialog.isNull(), "Finished workflow close did not delete the dialog; reopening would strand its Close action");
}

void dialogCancelLifecycle(
    const QString& root,
    const QString& game,
    const QString& runner,
    const QProcessEnvironment& environment) {
  auto running = read(root + "/state.json");
  running.insert("state", "running");
  running.insert("owner_released", false);
  running.remove("cancel_requested");
  auto initial_owner = hm::game_operation::Inspect(game.toStdString());
  require(initial_owner.ok() && initial_owner->has_value(), "Cancellation fixture requires durable workflow ownership");
  auto remote = **initial_owner;
  QMainWindow owner;
  auto* main_action = new QPushButton("Main window action", &owner);
  owner.setCentralWidget(main_action);
  owner.resize(800, 600);
  owner.show();
  int main_actions = 0;
  QObject::connect(main_action, &QPushButton::clicked, &owner, [&] { ++main_actions; });
  int reopen_count = 0;
  for (const QString scenario :
       {"button",
        "window",
        "escape",
        "cancel-error",
        "lost-cancel-response",
        "recovery",
        "status-unavailable",
        "cleanup-recovery",
        "cleanup-error",
        "controller-error",
        "controller-offline",
        "button"}) {
    require(!owner.findChild<QDialog*>("stitchingExperimentDialog"), "Reopen found a previously closing dialog");
    if (reopen_count) {
      auto lock = hm::game_operation::Lock::Acquire(game.toStdString());
      require(lock.ok() && (*lock)->Claim(remote).ok(), "Cannot claim the next isolated workflow fixture");
    }
    running.insert("epoch", reopen_count);
    running.insert("state", scenario == "recovery" ? "recovery" : "running");
    write(root + "/state.json", QJsonDocument(running).toJson());
    write(root + "/hold-owner-release", "held");
    const int cancellations = controllerCalls(root, "cancel");
    const int retries = controllerCalls(root, "retry");
    bool saw_release = false;
    bool retry_enabled_at_release = false;
    QPointer<StitchingExperimentDialog> dialog = new StitchingExperimentDialog(
        game, runner, root, game + "/_hockeymon/config.yaml", environment, 100, 1, "00:00:00", &owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->show();
    require(
        wait([&] {
          return widget<QComboBox>(*dialog, "stitchWorkflowStage")->count() > 4 &&
              widget<QPushButton>(*dialog, "cancelStitchExperimentsButton")->isEnabled();
        }),
        "Parent-owned monitor did not attach to the running workflow");
    require(QApplication::activeModalWidget() == dialog, "Fixture must exercise the real modal parent relationship");
    widget<QComboBox>(*dialog, "stitchWorkflowStage")->setCurrentIndex(0);
    auto* workflow = static_cast<StitchingWorkflow*>(widget<QWidget>(*dialog, "stitchingWorkflowPane"));
    const auto snapshot_changed = workflow->snapshotChanged;
    workflow->snapshotChanged = [&, snapshot_changed](const QJsonObject& value) {
      snapshot_changed(value);
      if (value.value("owner_released").toBool()) {
        saw_release = true;
        auto* retry = widget<QPushButton>(*dialog, "retryStitchWorkflowStageButton");
        retry_enabled_at_release |= retry->isEnabled();
        retry->click();
      }
    };
    const QString route = scenario == "window" || scenario == "escape" ? scenario : "button";
    closeWorkflow(*dialog, route, false);
    require(
        dialog && dialog->isVisible() && controllerCalls(root, "cancel") == cancellations,
        "Keeping the workflow running closed its dialog or sent cancellation");
    QPointer<QDialog> expanded;
    if (reopen_count % 2 == 0) {
      widget<QPushButton>(*dialog, "expandStitchWorkflowButton")->click();
      expanded = widget<QDialog>(*dialog, "stitchWorkflowExpandedWindow");
      require(wait([&] { return expanded && expanded->isVisible(); }), "Expanded monitor did not open");
    }
    int finished = 0;
    QObject::connect(dialog, &QDialog::finished, &owner, [&](int) { ++finished; });
    const bool retry = scenario == "cancel-error" || scenario == "lost-cancel-response";
    if (retry)
      write(root + (scenario == "cancel-error" ? "/reject-cancel" : "/lose-cancel-response"), "reject");
    closeWorkflow(*dialog, route, true);
    if (retry) {
      require(
          wait([&] {
            return widget<QLabel>(*dialog, "stitchExperimentStatus")
                ->text()
                .contains(scenario == "cancel-error" ? "Cancellation rejected" : "Cancellation response unavailable");
          }),
          "Cancellation failure was not reported for explicit retry");
      require(dialog->isVisible() && finished == 0, "Cancellation failure silently closed the dialog");
      QFile::remove(root + "/reject-cancel");
      QFile::remove(root + "/lose-cancel-response");
      closeWorkflow(*dialog, route, true);
    }
    int expected_cancellations = cancellations + (retry ? 2 : 1);
    // The controller writes its state before the response arrives. Inject
    // post-cancellation failures only after the command callback completes.
    require(
        wait([&] {
          return !workflow->busy() && controllerCalls(root, "cancel") == expected_cancellations &&
              read(root + "/state.json").value("cancel_requested").toBool();
        }),
        "Confirmed close did not request workflow cancellation");
    if (scenario == "status-unavailable" || scenario == "cleanup-recovery" || scenario == "cleanup-error" ||
        scenario == "controller-error" || scenario == "controller-offline") {
      QString expected_error;
      if (scenario == "status-unavailable")
        write(root + "/status-unavailable", "2");
      else {
        auto stalled = read(root + "/state.json");
        if (scenario == "cleanup-recovery")
          stalled.insert("state", "recovery");
        else if (scenario == "controller-error") {
          expected_error = "Fixture controller could not reconcile ownership release";
          stalled.insert("controller_error", QJsonObject{{"error", expected_error}});
        } else if (scenario == "controller-offline") {
          expected_error = "Fixture controller heartbeat unavailable";
          stalled.insert("controller_online", false);
          stalled.insert("controller_warning", QJsonObject{{"error", expected_error}});
        } else
          stalled.insert("cancel_error", QJsonObject{{"error", "Fixture cleanup could not confirm resource release"}});
        write(root + "/state.json", QJsonDocument(stalled).toJson());
      }
      const bool retry_available = wait([&] {
        return widget<QPushButton>(*dialog, "closeStitchExperimentButton")->isEnabled() &&
            widget<QPushButton>(*dialog, "cancelStitchExperimentsButton")->isEnabled() &&
            (expected_error.isEmpty() ||
             widget<QLabel>(*dialog, "stitchExperimentStatus")->text().contains(expected_error));
      });
      if (!retry_available)
        std::cerr << "Close recovery scenario " << scenario.toStdString() << ": "
                  << widget<QLabel>(*dialog, "stitchExperimentStatus")->text().toStdString() << '\n'
                  << "Close enabled=" << widget<QPushButton>(*dialog, "closeStitchExperimentButton")->isEnabled()
                  << " Cancel enabled=" << widget<QPushButton>(*dialog, "cancelStitchExperimentsButton")->isEnabled()
                  << " Workflow busy=" << workflow->busy() << " active=" << workflow->active() << '\n'
                  << QJsonDocument(read(root + "/state.json")).toJson(QJsonDocument::Compact).toStdString() << '\n';
      require(
          retry_available,
          "Monitoring or cleanup failure stranded the pending close instead of allowing explicit cancellation retry");
      require(
          dialog->isVisible() && finished == 0 && controllerCalls(root, "cancel") == expected_cancellations,
          "Monitoring recovery dismissed the dialog or retried cancellation without an explicit action");
      QFile::remove(root + "/status-unavailable");
      auto recovered = read(root + "/state.json");
      recovered.insert("state", "cancelled");
      recovered.remove("cancel_error");
      recovered.remove("controller_error");
      recovered.remove("controller_warning");
      recovered.remove("controller_online");
      write(root + "/state.json", QJsonDocument(recovered).toJson());
      closeWorkflow(*dialog, route, true);
      ++expected_cancellations;
      require(
          wait([&] { return controllerCalls(root, "cancel") == expected_cancellations; }),
          "Explicit close retry did not reach the recovered cancellation controller");
    }
    const int polls = controllerCalls(root, "status");
    require(wait([&] { return controllerCalls(root, "status") >= polls + 3; }), "Cleanup monitoring stopped");
    require(
        dialog && dialog->isVisible() && finished == 0 && QApplication::activeModalWidget() == dialog &&
            controllerCalls(root, "cancel") == expected_cancellations,
        "Terminal cancellation closed before owner release or repeatedly sent cancellation");
    closeWorkflow(*dialog, route, false, false);
    require(controllerCalls(root, "cancel") == expected_cancellations, "Repeated Close resent pending cancellation");
    const auto retained_owner = hm::game_operation::Inspect(game.toStdString());
    require(
        retained_owner.ok() && retained_owner->has_value() && (**retained_owner).token == remote.token,
        "Closing bypassed the workflow's retained cleanup ownership");
    auto terminal = read(root + "/state.json");
    terminal.remove("owner_released");
    write(root + "/state.json", QJsonDocument(terminal).toJson());
    const int missing_release_polls = controllerCalls(root, "status");
    require(
        wait([&] { return controllerCalls(root, "status") >= missing_release_polls + 2; }) && dialog->isVisible(),
        "Missing release attestation was treated as completed cleanup");
    const auto screenshots = qEnvironmentVariable("HSTREAM_WORKFLOW_CLOSE_SCREENSHOT_DIR");
    if (!screenshots.isEmpty() && !expanded)
      require(dialog->grab().save(screenshots + "/waiting-for-cleanup.png"), "Cannot capture cleanup wait");
    {
      auto lock = hm::game_operation::Lock::Acquire(game.toStdString());
      require(lock.ok() && (*lock)->Release(remote.token, remote.revision, true).ok(), "Cannot finish fixture cleanup");
    }
    QFile::remove(root + "/hold-owner-release");
    terminal.insert("owner_released", true);
    write(root + "/state.json", QJsonDocument(terminal).toJson());
    waitForDeleted(dialog);
    require(expanded.isNull() && finished == 1, "Cancellation close left an expanded window or duplicated completion");
    require(!QApplication::activeModalWidget(), "Completed cancellation left the main window modal-blocked");
    main_action->click();
    require(main_actions == ++reopen_count, "Main window controls did not return after verified cancellation");
    require(controllerCalls(root, "cancel") == expected_cancellations, "Final cleanup resubmitted cancellation");
    require(
        saw_release && !retry_enabled_at_release && controllerCalls(root, "retry") == retries &&
            read(root + "/state.json").value("epoch").toInt() == reopen_count - 1,
        "Deferred close briefly enabled Retry or admitted a new epoch after cleanup released the workflow");
  }
}

void dialogCloseDuringSubmission(const QString& root) {
  auto environment = QProcessEnvironment::systemEnvironment();
  environment.insert("WORKFLOW_TEST_ROOT", root);
  QMainWindow owner;
  owner.resize(800, 600);
  owner.show();
  for (const QString scenario : {"accepted", "lost-response", "rejected"}) {
    const QString game = root + "/close-during-submit-" + scenario;
    QDir().mkpath(game + "/_hockeymon");
    write(
        game + "/_hockeymon/config.yaml",
        ("game:\n  videos:\n    left: [left.mp4]\n    right: [right.mp4]\nworkflow:\n  registry: " + root +
         "\n  poll_interval_ms: 100\n")
            .toUtf8());
    write(game + "/left.mp4", "left");
    write(game + "/right.mp4", "right");
    QFile::remove(root + "/lose-submit-response");
    QFile::remove(root + "/skip-submit-status-failures");
    QFile::remove(root + "/reject-submit");
    QFile::remove(root + "/status-unavailable");
    write(root + "/hold-submit", "held");
    write(root + "/hold-owner-release", "held");
    if (scenario != "accepted")
      write(root + (scenario == "rejected" ? "/reject-submit" : "/lose-submit-response"), "fixture");
    if (scenario == "lost-response")
      write(root + "/skip-submit-status-failures", "fixture");
    const int submissions = controllerCalls(root, "submit");
    const int cancellations = controllerCalls(root, "cancel");
    QPointer<StitchingExperimentDialog> dialog = new StitchingExperimentDialog(
        game, "/bin/false", root, game + "/_hockeymon/config.yaml", environment, 100, 1, "00:00:00", &owner);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowModality(Qt::WindowModal);
    dialog->show();
    auto* profile = widget<QComboBox>(*dialog, "stitchWorkflowProfile");
    auto* start = widget<QPushButton>(*dialog, "startStitchExperimentBatchButton");
    widget<QPushButton>(*dialog, "addStitchExperimentsToBatchButton")->click();
    require(
        wait([&] { return start->isEnabled() && profile->count() == 2; }),
        "Pending submission fixture did not prepare");
    profile->setCurrentIndex(1);
    start->click();
    require(
        wait([&] { return controllerCalls(root, "submit") == submissions + 1; }), "Submission was not held in flight");
    closeWorkflow(*dialog, "button", true);
    QElapsedTimer held;
    held.start();
    require(wait([&] { return held.elapsed() >= 150; }), "Pending submission observation stalled");
    require(
        dialog && dialog->isVisible() && controllerCalls(root, "cancel") == cancellations,
        "Close raced cancellation against an unresolved submission or disappeared before admission resolved");
    QFile::remove(root + "/hold-submit");
    if (scenario != "rejected") {
      require(
          wait([&] {
            return controllerCalls(root, "cancel") == cancellations + 1 &&
                read(root + "/state.json").value("cancel_requested").toBool();
          }),
          "Pending Close lost its cancellation intent after submission acceptance or response loss");
      require(dialog && dialog->isVisible(), "Accepted submission closed before cancellation cleanup");
      auto terminal = read(root + "/state.json");
      terminal.insert("owner_released", true);
      write(root + "/state.json", QJsonDocument(terminal).toJson());
    }
    waitForDeleted(dialog);
    require(
        controllerCalls(root, "submit") == submissions + 1 &&
            controllerCalls(root, "cancel") == cancellations + (scenario == "rejected" ? 0 : 1),
        "Close resubmitted compute, duplicated cancellation or cancelled a definitely rejected submission");
    require(!QApplication::activeModalWidget(), "Resolved submission close kept the main window modal-blocked");
    QFile::remove(root + "/hold-owner-release");
  }
  QFile::remove(root + "/lose-submit-response");
  QFile::remove(root + "/skip-submit-status-failures");
  QFile::remove(root + "/reject-submit");
  QFile::remove(root + "/status-unavailable");
}

void dialogOwnership(const QString& root) {
  const auto status_calls = [&] {
    QFile calls(root + "/calls");
    require(calls.open(QIODevice::ReadOnly), "Cannot inspect workflow polling");
    return calls.readAll().count("status\n");
  };
  const auto retain_preview_tab = [&](StitchingExperimentDialog& current) {
    auto* tabs = widget<QTabWidget>(current, "stitchExperimentViewTabs");
    tabs->setCurrentIndex(0);
    const int initial_calls = status_calls();
    require(wait([&] { return status_calls() >= initial_calls + 3; }), "Workflow did not continue polling");
    require(tabs->currentIndex() == 0, "Workflow polling repeatedly stole the selected preview tab");
    require(
        !widget<QTimer>(current, "stitchWorkflowAnimation")->isActive(),
        "Preview tab left the hidden workflow's animation timer active during polling");
  };
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
  auto* tabs = widget<QTabWidget>(dialog, "stitchExperimentViewTabs");
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
  require(tabs->currentIndex() == 1, "Local batch start did not open its Workflow tab");
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
  tabs->setCurrentIndex(0);
  start->click();
  require(tabs->currentIndex() == 1, "Distributed batch start did not open its Workflow tab");
  require(
      wait([&] { return widget<QComboBox>(dialog, "stitchWorkflowStage")->count() > 4 && cancel->isEnabled(); }),
      "Dialog did not submit a dependency graph");
  require(
      widget<QComboBox>(dialog, "stitchWorkflowStage")->currentText().startsWith("Calibrate"),
      "Selected row did not select its solve stage");
  require(
      rows->isEnabled() && widget<QPushButton>(dialog, "viewStitchExperimentRunnerLogButton")->isEnabled(),
      "Running workflow blocked stage inspection");
  auto* stage_control = widget<QComboBox>(dialog, "stitchWorkflowStage");
  stage_control->setCurrentIndex(0);
  const auto selected_stage = stage_control->currentData();
  auto* video = widget<QWidget>(dialog, "stitchExperimentVideo");
  const WId target = video->winId();
  retain_preview_tab(dialog);
  require(
      stage_control->currentData() == selected_stage && video->winId() == target,
      "Polling another tab lost workflow stage selection or replaced the preview target");
  for (const int index : {0, 1}) {
    tabs->setCurrentIndex(index);
    for (const auto modifiers : {Qt::NoModifier, Qt::ControlModifier}) {
      QWheelEvent wheel(
          QPointF(4, 4),
          QPointF(tabs->tabBar()->mapToGlobal(QPoint(4, 4))),
          QPoint(0, -20),
          QPoint(0, -120),
          Qt::NoButton,
          modifiers,
          Qt::NoScrollPhase,
          false);
      QApplication::sendEvent(tabs->tabBar(), &wheel);
      require(tabs->currentIndex() == index, "Wheel input changed the experiment view tab");
    }
  }
  auto* dialog_graph = widget<QScrollArea>(dialog, "stitchWorkflowGraphScroll");
  widget<QPushButton>(dialog, "expandStitchWorkflowButton")->click();
  require(wait([&] { return dialog_graph->window() != &dialog; }), "Workflow did not open its expanded monitor");
  tabs->setCurrentIndex(0);
  require(
      wait([&] {
        return !dialog.findChild<QDialog*>("stitchWorkflowExpandedWindow") && dialog_graph->window() == &dialog;
      }),
      "Changing to preview left a detached workflow monitor open");
  tabs->setCurrentIndex(1);
  require(stage_control->currentData() == selected_stage, "Restoring Workflow lost the inspected stage");
  auto* raw_log = widget<QPlainTextEdit>(dialog, "stitchExperimentLog");
  require(raw_log->toPlainText().isEmpty() && raw_log->isHidden(), "Empty local output must not crowd managed stages");
  require(
      wait([&] { return dialog_graph->viewport()->height() >= 180; }),
      "Managed Workflow did not use the empty process log's space for its graph");
  raw_log->appendPlainText("Retained local runner output");
  require(wait([&] { return raw_log->isVisible(); }), "Managed Workflow hid nonempty local runner output");
  raw_log->clear();
  require(wait([&] { return raw_log->isHidden(); }), "Clearing managed local output did not restore graph space");
  const QSize initial_size = dialog.size();
  const int initial_graph_height = dialog_graph->viewport()->height();
  dialog.resize(initial_size.width(), initial_size.height() + 400);
  const bool graph_grew = wait([&] { return dialog_graph->viewport()->height() >= initial_graph_height + 50; });
  std::cout << "Dialog height " << initial_size.height() << " -> " << dialog.height() << "; workflow graph "
            << initial_graph_height << " -> " << dialog_graph->viewport()->height() << '\n';
  const auto large_screenshot = qEnvironmentVariable("HSTREAM_WORKFLOW_DIALOG_SCREENSHOT");
  if (!large_screenshot.isEmpty())
    require(dialog.grab().save(large_screenshot + ".large.png"), "Could not capture enlarged workflow dialog");
  require(graph_grew, "Resizing Stitching Experiments did not allocate more space to its workflow graph");
  const int enlarged_graph_height = dialog_graph->viewport()->height();
  dialog.resize(initial_size);
  require(
      wait([&] { return dialog_graph->viewport()->height() < enlarged_graph_height; }),
      "Restoring the dialog size did not restore its workflow viewport");
  dialog.resize(960, 680);
  QApplication::processEvents();
  auto* dialog_monitor = widget<QScrollArea>(dialog, "stitchWorkflowMonitorScroll");
  auto* dialog_retry = widget<QPushButton>(dialog, "retryStitchWorkflowStageButton");
  dialog_monitor->ensureWidgetVisible(dialog_retry);
  require(
      wait([&] { return QRegion(dialog_retry->rect()).subtracted(dialog_retry->visibleRegion()).isEmpty(); }),
      "The smallest experiment dialog clipped its workflow retry control");
  const int compact_monitor_height = dialog_monitor->height();
  dialog.resize(initial_size);
  require(
      wait([&] { return dialog_monitor->height() > compact_monitor_height; }),
      "Restoring the normal dialog size did not expand its workflow monitor");
  dialog_monitor->verticalScrollBar()->setValue(0);
  const auto screenshot = qEnvironmentVariable("HSTREAM_WORKFLOW_DIALOG_SCREENSHOT");
  if (!screenshot.isEmpty())
    require(dialog.grab().save(screenshot), "Could not capture distributed experiment dialog");
  QFile before(root + "/calls");
  require(before.open(QIODevice::ReadOnly), "Cannot read controller calls");
  const auto calls = before.readAll();
  before.close();
  const auto running_snapshot = read(root + "/state.json");
  closeWorkflow(dialog, "button", true);
  require(wait([&] { return !dialog.isVisible(); }), "Confirmed workflow cancellation did not close the dialog");
  QFile after(root + "/calls");
  require(after.open(QIODevice::ReadOnly), "Cannot reread controller calls");
  require(after.readAll().count("cancel") == calls.count("cancel") + 1, "Confirmed Close did not cancel exactly once");
  write(root + "/state.json", QJsonDocument(running_snapshot).toJson());
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
  const auto index_path = QString::fromStdString((store->directory / "index.yaml").string());
  QFile index(index_path);
  require(index.open(QIODevice::ReadOnly), "Cannot snapshot unbound catalog");
  const auto unbound_index = index.readAll();
  index.close();
  for (const bool unreadable_catalog : {false, true}) {
    if (unreadable_catalog)
      write(index_path, "[invalid catalog\n");
    StitchingExperimentDialog reopened(
        game, runner, root, game + "/_hockeymon/config.yaml", environment, 100, 1, "00:00:00");
    reopened.show();
    require(
        wait([&] {
          return widget<QComboBox>(reopened, "stitchWorkflowStage")->count() > 4 &&
              widget<QPushButton>(reopened, "cancelStitchExperimentsButton")->isEnabled();
        }),
        "Durable owner did not restore monitoring before catalog binding or with unreadable retained data");
    require(
        widget<QTabWidget>(reopened, "stitchExperimentViewTabs")->currentIndex() == 1,
        "Attaching an active workflow must open its Workflow tab");
    retain_preview_tab(reopened);
    require(
        !widget<QPushButton>(reopened, "addStitchExperimentsToBatchButton")->isEnabled() &&
            !widget<QPushButton>(reopened, "startStitchExperimentBatchButton")->isEnabled(),
        "Unbound workflow monitoring permitted a conflicting batch");
    closeWorkflow(reopened, "button", false);
    require(reopened.isVisible(), "Declining cancellation closed an unbound owner monitor");
  }
  write(index_path, unbound_index);
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
    auto* reopened_tabs = widget<QTabWidget>(reopened, "stitchExperimentViewTabs");
    const QPoint preview_tab = reopened_tabs->tabBar()->tabRect(0).center();
    for (const auto event : {QEvent::MouseButtonPress, QEvent::MouseButtonRelease}) {
      QMouseEvent click(
          event,
          preview_tab,
          reopened_tabs->tabBar()->mapToGlobal(preview_tab),
          Qt::LeftButton,
          event == QEvent::MouseButtonPress ? Qt::LeftButton : Qt::NoButton,
          Qt::NoModifier);
      QApplication::sendEvent(reopened_tabs->tabBar(), &click);
    }
    require(
        wait([&] {
          return widget<QComboBox>(reopened, "stitchWorkflowStage")->count() > 4 &&
              widget<QPushButton>(reopened, "cancelStitchExperimentsButton")->isEnabled();
        }),
        "Workflow-owned game did not reopen and attach to running stages");
    require(
        reopened_tabs->currentIndex() == 0,
        "First workflow status overrode the user's Preview choice during attachment");
    retain_preview_tab(reopened);
    const auto owner = hm::game_operation::Inspect(store->game_directory);
    require(
        owner.ok() && owner->has_value() && (**owner).id == id && (**owner).revision == remote.revision,
        "Read-only reopen changed durable workflow admission");
    require(
        main_config.open(QIODevice::ReadOnly) && main_config.readAll() == before_reopen,
        "Read-only workflow reopen rewrote the main configuration");
    closeWorkflow(reopened, "button", false);
    require(reopened.isVisible(), "Declining cancellation closed the reopened monitor");
  }
  dialogCancelLifecycle(root, game, runner, environment);
}

void killedExperimentGuard(const QString& root) {
  const auto game = root + "/lost-guard-game";
  QDir().mkpath(game + "/_hockeymon");
  write(game + "/_hockeymon/config.yaml", "game:\n  videos:\n    left: [left.mp4]\n    right: [right.mp4]\n");
  write(game + "/left.mp4", "left");
  write(game + "/right.mp4", "right");
  const auto runner = root + "/lost-guard-runner";
  write(runner, "#!/bin/sh\nexec sleep 30\n");
  QFile::setPermissions(runner, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
  StitchingExperimentDialog dialog(
      game,
      runner,
      root,
      game + "/_hockeymon/config.yaml",
      QProcessEnvironment::systemEnvironment(),
      100,
      1,
      "00:00:00");
  dialog.show();
  auto* add = widget<QPushButton>(dialog, "addStitchExperimentsToBatchButton");
  auto* start = widget<QPushButton>(dialog, "startStitchExperimentBatchButton");
  auto* cancel = widget<QPushButton>(dialog, "cancelStitchExperimentsButton");
  add->click();
  require(wait([&] { return start->isEnabled(); }), "Cannot prepare lost-guard experiment");
  start->click();
  const auto store = OpenStitchingExperimentStore(game.toStdString());
  require(store.ok(), "Cannot inspect lost-guard experiment store");
  int64_t process = 0;
  require(
      wait([&] {
        auto catalog = LoadStitchingExperimentStore(*store);
        if (catalog.ok() && !catalog->experiments.empty())
          process = catalog->experiments.front().process_session_id;
        return process > 0 && cancel->isEnabled();
      }),
      "Guard process ownership was not durably recorded");
  const auto owned = hm::game_operation::Inspect(game.toStdString());
  require(owned.ok() && owned->has_value(), "Lost-guard fixture has no game admission");
  QFile children(QString("/proc/%1/task/%1/children").arg(process));
  pid_t worker = 0;
  require(
      wait([&] {
        if (!children.open(QIODevice::ReadOnly))
          return false;
        worker = static_cast<pid_t>(children.readAll().trimmed().toLongLong());
        children.close();
        return worker > 0 && ::getpgid(worker) == worker;
      }),
      "Cannot identify exact test-owned worker group");
  // The guard stays outside the worker group. Killing only the guard must
  // retain admission; stopping its exact worker cannot replace missing proof.
  require(::kill(-static_cast<pid_t>(process), SIGKILL) == 0, "Cannot kill test-owned guard session");
  require(wait([&] { return !cancel->isEnabled(); }, 10000), "Lost guard did not finish its fenced UI cleanup");
  const auto retained = hm::game_operation::Inspect(game.toStdString());
  require(::kill(-worker, SIGKILL) == 0, "Cannot stop exact lost-guard worker group");
  require(
      retained.ok() && retained->has_value() && (**retained).token == (**owned).token,
      "Killed experiment guard released durable admission");
  require(
      !add->isEnabled() && !start->isEnabled() &&
          !widget<QPushButton>(dialog, "removeStitchExperimentFromBatchButton")->isEnabled() &&
          !widget<QPushButton>(dialog, "applyStitchExperimentButton")->isEnabled(),
      "Missing experiment receipt permitted new work, removal or publication");
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
    profileResponseBounds(temporary.path(), YAML::Clone(baseline->values));
    dialogOwnership(temporary.path());
    dialogCloseDuringSubmission(temporary.path());
    killedExperimentGuard(temporary.path());
    std::cout
        << "Workflow DAG, async UI, retry/log history, wheel routing, close confirmation and verified cancellation passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
