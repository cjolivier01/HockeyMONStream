#include "src/apps/hstream-ui/StitchingWorkflow.h"

#include "hstream/src/libs/common/UserConfig.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/ExperimentTaskRuntime.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonParseError>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QSaveFile>
#include <QtCore/QSignalBlocker>
#include <QtCore/QStandardPaths>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtGui/QDesktopServices>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialog>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace {
constexpr int kMaxWorkflowDocumentBytes = 8 * 1024 * 1024;

QString text(const std::string& value) {
  return QString::fromStdString(value);
}
QJsonValue jsonValue(const YAML::Node& value, int depth = 0) {
  if (depth > 64)
    throw std::runtime_error("User configuration exceeds the workflow nesting limit");
  if (!value || value.IsNull())
    return QJsonValue(QJsonValue::Null);
  if (value.IsMap()) {
    QJsonObject result;
    for (const auto& child : value)
      result.insert(text(child.first.as<std::string>()), jsonValue(child.second, depth + 1));
    return result;
  }
  if (value.IsSequence()) {
    QJsonArray result;
    for (const auto& child : value)
      result.append(jsonValue(child, depth + 1));
    return result;
  }
  // Preserve exact scalar spelling. Native YAML readers perform the same typed
  // conversions as for the original overlay; quoted paths remain untouched.
  return text(value.as<std::string>());
}
QJsonObject descriptor(const StitchingExperimentWorkspace& workspace) {
  return {
      {"root", text(workspace.root.string())},
      {"game_directory", text(workspace.game_directory.string())},
      {"game_id", text(workspace.game_id)},
      {"invalidation_id", text(workspace.invalidation_id)},
      {"settings", hm::experiment::settings_json(workspace.settings)}};
}
QString keyFor(const StitchingExperimentStore& store, const StoredStitchingExperiment& row) {
  return text(row.workspace.game_directory.lexically_relative(store.directory).generic_string());
}
QString digest(const QJsonObject& value) {
  return QCryptographicHash::hash(QJsonDocument(value).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256)
      .toHex();
}
QJsonObject reference(const QString& stage) {
  return {{"stage", stage}, {"output", "workspace"}};
}
bool terminal(const QString& state) {
  return state == "succeeded" || state == "failed" || state == "cancelled";
}
bool unsuccessful(const QString& state) {
  return state == "failed" || state == "unavailable" || state == "blocked" || state == "cancelled";
}

QString stageLabel(const QJsonObject& node) {
  const auto label = node.value("label").toString();
  const auto kind = node.value("kind").toString();
  const auto separator = label.indexOf(" · ");
  const auto prefix = label.left(separator);
  QString replacement;
  if (kind == "experiment-create" && prefix == "Prepare")
    replacement = "Set up experiment files (CPU)";
  else if (kind == "experiment-create" && prefix == "Prepare retained frames")
    replacement = "Set up retained frames (CPU)";
  else if (kind == "experiment-baseline" && prefix == "Baseline")
    replacement = "Initial calibration";
  else if (kind == "experiment-scan" && prefix == "Scan")
    replacement = "Find player-rich frames";
  else if (kind == "experiment-materialize" && prefix == "Materialize")
    replacement = "Extract selected frames";
  else if (kind == "experiment-reuse-selection" && prefix == "Selection")
    replacement = "Use shared frames (CPU)";
  else if (kind == "experiment-run" && prefix == "Solve")
    replacement = "Calibrate";
  // Old sealed plans keep their original identities and labels on disk.
  return replacement.isEmpty() ? label : replacement + (separator < 0 ? "" : " · #" + label.mid(separator + 3));
}
QString stagePurpose(const QJsonObject& node) {
  const auto kind = node.value("kind").toString();
  if (kind == "experiment-create")
    return stageLabel(node).contains("retained frames")
        ? "Set up private experiment files using the retained frame selection. CPU only; no new frame search."
        : "Set up private experiment files and link the source videos. CPU only; no GPU is reserved.";
  if (kind == "experiment-baseline")
    return "Calibrate the cameras and create the rink mask needed to find player-rich frames.";
  if (kind == "experiment-scan")
    return "Find source frames with the most players inside the rink, using the initial calibration.";
  if (kind == "experiment-materialize")
    return "Extract the selected source-frame pairs once for all dependent calibrations.";
  if (kind == "experiment-reuse-selection")
    return "Copy the shared frame selection into this experiment. CPU only; no new frame search.";
  if (kind == "experiment-run")
    return "Calibrate this candidate using its selected frames and control-point limit.";
  return {};
}
QString stageDescription(const QJsonObject& node, const QJsonArray& nodes) {
  QStringList lines{stagePurpose(node)};
  if (node.contains("resources")) {
    const auto gpus = node.value("resources").toObject().value("gpus").toInt();
    lines << (gpus == 0 ? "Allocation: CPU only (no GPU)." : QString("Allocation: %1 GPU(s).").arg(gpus));
  }
  QStringList prerequisites, dependents;
  const auto dependencies = node.value("dependencies").toArray();
  for (const auto& value : nodes) {
    const auto other = value.toObject();
    if (dependencies.contains(other.value("id")))
      prerequisites << stageLabel(other);
    if (other.value("dependencies").toArray().contains(node.value("id")))
      dependents << stageLabel(other);
  }
  lines << "Prerequisites: " + (prerequisites.isEmpty() ? "None" : prerequisites.join(", "));
  lines << "Used by: " + (dependents.isEmpty() ? "None" : dependents.join(", "));
  return lines.join("\n");
}

class StageGraph final : public QWidget {
 public:
  explicit StageGraph(QWidget* parent) : QWidget(parent) {
    setObjectName("stitchWorkflowGraph");
    setMinimumWidth(220);
    // State uses an icon and the existing border; card backgrounds and text
    // follow the theme. Keep these colors aligned with the browser.
    setStyleSheet(
        "QPushButton { background-color: palette(button); color: palette(button-text);"
        " border: 2px solid palette(mid); border-radius: 4px; padding: 4px; }"
        "QPushButton[workflowState='running'] { border-color: #68b8f0; }"
        "QPushButton[workflowState='succeeded'], QPushButton[workflowState='reused']"
        " { border-color: #71ba8c; }"
        "QPushButton[workflowState='failed'], QPushButton[workflowState='recovery']"
        " { border-color: #ec939b; }"
        "QPushButton:checked { border-color: palette(button-text); }"
        "QPushButton[workflowState='running']:checked, QPushButton[workflowState='succeeded']:checked,"
        " QPushButton[workflowState='reused']:checked, QPushButton[workflowState='failed']:checked,"
        " QPushButton[workflowState='recovery']:checked { border-color: #ffd166; }"
        "QPushButton:focus { border-style: dashed; }");
    animation_ = new QTimer(this);
    animation_->setObjectName("stitchWorkflowAnimation");
    animation_->setInterval(40);
    QObject::connect(animation_, &QTimer::timeout, this, [this] {
      angle_ = (angle_ + 12) % 360;
      animateRunning();
    });
  }
  std::function<void(const QString&)> selected;
  void showStages(const QJsonArray& nodes, const QString& current) {
    const QByteArray identity = QJsonDocument(nodes).toJson(QJsonDocument::Compact) + current.toUtf8();
    if (identity == identity_)
      return;
    identity_ = identity;
    nodes_ = nodes;
    current_ = current;
    for (auto* button : buttons_)
      delete button;
    buttons_.clear();
    running_.clear();
    for (const auto& value : nodes_) {
      const auto node = value.toObject();
      const auto id = node.value("id").toString();
      const auto state = node.value("state").toString();
      const auto icon = state == "running"            ? ActionIcon::Running
          : state == "succeeded" || state == "reused" ? ActionIcon::Succeeded
          : state == "failed" || state == "recovery"  ? ActionIcon::Failed
                                                      : ActionIcon::Inspect;
      auto* button = new QPushButton(action_icon(icon), "", this);
      button->setIconSize(QSize(20, 20));
      if (state == "running")
        running_.push_back(button);
      button->setObjectName("stitchWorkflowNode_" + id);
      button->setProperty("workflowState", node.value("state").toString());
      button->setAccessibleName(stageLabel(node) + " · " + node.value("state").toString());
      button->setToolTip(stageDescription(node, nodes_) + "\n" + node.value("reason").toString());
      button->setCheckable(true);
      button->setChecked(id == current);
      button->show();
      QObject::connect(button, &QPushButton::clicked, this, [this, id] {
        if (selected)
          selected(id);
      });
      buttons_.push_back(button);
    }
    layoutStages();
    updateAnimation();
  }

 protected:
  void showEvent(QShowEvent* event) override {
    QWidget::showEvent(event);
    updateAnimation();
  }
  void hideEvent(QHideEvent* event) override {
    animation_->stop();
    QWidget::hideEvent(event);
  }
  void resizeEvent(QResizeEvent* event) override {
    QWidget::resizeEvent(event);
    layoutStages();
  }
  void paintEvent(QPaintEvent*) override {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    for (const auto& edge : edges_) {
      QColor color = palette().color(edge.selected ? QPalette::Highlight : QPalette::Text);
      if (!edge.selected)
        color.setAlpha(150);
      painter.setPen(QPen(color, edge.selected ? 2.5 : 1.5));
      painter.drawPath(edge.path);
      painter.drawLine(edge.end, edge.end + QPointF(-4, -5));
      painter.drawLine(edge.end, edge.end + QPointF(4, -5));
    }
  }

 private:
  void updateAnimation() {
    if (isVisible() && !running_.empty()) {
      animateRunning();
      if (!animation_->isActive())
        animation_->start();
    } else {
      animation_->stop();
    }
  }
  void animateRunning() {
    // Rotate only the small shared glyph; animation never rebuilds the graph,
    // moves controls or touches preview frames. Hidden tabs stop the timer.
    const auto ratio = devicePixelRatioF();
    QPixmap frame(qRound(20 * ratio), qRound(20 * ratio));
    frame.setDevicePixelRatio(ratio);
    frame.fill(Qt::transparent);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.translate(10, 10);
    painter.rotate(angle_);
    painter.drawPixmap(QPointF(-10, -10), fan_.pixmap(QSize(20, 20), ratio));
    painter.end();
    QIcon icon;
    for (auto mode : {QIcon::Normal, QIcon::Active, QIcon::Selected})
      for (auto state : {QIcon::Off, QIcon::On})
        icon.addPixmap(frame, mode, state);
    QPixmap disabled = frame;
    QPainter fade(&disabled);
    fade.setCompositionMode(QPainter::CompositionMode_DestinationIn);
    fade.fillRect(disabled.rect(), QColor(255, 255, 255, 115));
    fade.end();
    for (auto state : {QIcon::Off, QIcon::On})
      icon.addPixmap(disabled, QIcon::Disabled, state);
    for (auto* button : running_)
      button->setIcon(icon);
  }
  QTimer* animation_{nullptr};
  QIcon fan_{action_icon(ActionIcon::Running)};
  int angle_{0};
  std::vector<QPushButton*> running_;
  struct Edge {
    QPainterPath path;
    QPointF end;
    bool selected;
  };
  void layoutStages() {
    if (buttons_.empty())
      return;
    std::map<QString, int> levels;
    for (int pass = 0; pass < nodes_.size(); ++pass) {
      bool changed = false;
      for (const auto& value : nodes_) {
        const auto node = value.toObject();
        const auto id = node.value("id").toString();
        if (levels.count(id))
          continue;
        bool ready = true;
        int level = 0;
        for (const auto& dependency : node.value("dependencies").toArray()) {
          const auto found = levels.find(dependency.toString());
          if (found == levels.end()) {
            ready = false;
            break;
          }
          level = std::max(level, found->second + 1);
        }
        if (ready) {
          levels[id] = level;
          changed = true;
        }
      }
      if (!changed)
        break;
    }
    // Place independent CPU setup just above its first consumer. This changes
    // only presentation: every stage and its actual dependency edges remain.
    for (const auto& value : nodes_) {
      const auto node = value.toObject();
      if (node.value("kind") != "experiment-create" || !node.value("dependencies").toArray().isEmpty())
        continue;
      int first = nodes_.size();
      for (const auto& child : nodes_)
        if (child.toObject().value("dependencies").toArray().contains(node.value("id")))
          first = std::min(first, levels[child.toObject().value("id").toString()]);
      if (first < nodes_.size())
        levels[node.value("id").toString()] = std::max(0, first - 1);
    }
    std::map<int, std::vector<int>> peers;
    for (int index = 0; index < nodes_.size(); ++index)
      peers[levels[nodes_[index].toObject().value("id").toString()]].push_back(index);
    const int available = std::max(180, width() - 40);
    const int columns = std::max(1, (available + 20) / 200);
    std::map<QString, QRect> rectangles;
    std::map<QString, int> level_top;
    int top = 10;
    for (const auto& [level, indices] : peers) {
      const int group_top = top;
      for (size_t first = 0; first < indices.size(); first += columns) {
        const int count = std::min<size_t>(columns, indices.size() - first);
        const int card_width = std::min(250, (available - (count - 1) * 20) / count);
        const int left = (width() - (card_width * count + (count - 1) * 20)) / 2;
        int row_height = 60;
        for (int column = 0; column < count; ++column) {
          const auto index = indices[first + column];
          const auto node = nodes_[index].toObject();
          auto* button = buttons_[index];
          QStringList lines;
          QString line;
          for (const auto& word : stageLabel(node).split(' ')) {
            const auto next = line.isEmpty() ? word : line + " " + word;
            if (!line.isEmpty() && button->fontMetrics().horizontalAdvance(next) > card_width - 40) {
              lines << line;
              line = word;
            } else {
              line = next;
            }
          }
          lines << line << node.value("state").toString();
          button->setText(lines.join("\n"));
          row_height = std::max(row_height, int(lines.size()) * button->fontMetrics().lineSpacing() + 18);
          const QRect bounds(left + column * (card_width + 20), top, card_width, row_height);
          rectangles[node.value("id").toString()] = bounds;
          level_top[node.value("id").toString()] = group_top;
        }
        for (int column = 0; column < count; ++column) {
          const auto index = indices[first + column];
          auto& bounds = rectangles[nodes_[index].toObject().value("id").toString()];
          bounds.setHeight(row_height);
          buttons_[index]->setGeometry(bounds);
        }
        top += row_height + 28;
      }
    }
    edges_.clear();
    for (const auto& value : nodes_) {
      const auto node = value.toObject();
      const auto id = node.value("id").toString();
      const auto target = rectangles.at(id);
      for (const auto& dependency : node.value("dependencies").toArray()) {
        const auto parent = dependency.toString();
        if (!rectangles.count(parent))
          continue;
        const auto source = rectangles.at(parent);
        const QPointF start(source.center().x(), source.bottom() + 1);
        const QPointF end(target.center().x(), target.top() - 1);
        QPainterPath path(start);
        if (target.top() > level_top[id] || levels[id] > levels[parent] + 1) {
          // Wrapped peers and skipped levels use an outside gutter, never a
          // line through an unrelated stage button.
          const int gutter = start.x() < width() / 2 ? 8 : width() - 8;
          path.lineTo(start.x(), source.bottom() + 10);
          path.lineTo(gutter, source.bottom() + 10);
          path.lineTo(gutter, target.top() - 12);
          path.lineTo(end.x(), target.top() - 12);
          path.lineTo(end);
        } else {
          const qreal middle = (start.y() + end.y()) / 2;
          path.cubicTo(start.x(), middle, end.x(), middle, end.x(), end.y());
        }
        edges_.push_back({path, end, id == current_ || parent == current_});
      }
    }
    setMinimumHeight(top - 18);
    update();
  }
  QByteArray identity_;
  QJsonArray nodes_;
  QString current_;
  std::vector<QPushButton*> buttons_;
  std::vector<Edge> edges_;
};
} // namespace

absl::StatusOr<QJsonObject> BuildStitchingWorkflowPlan(
    const StitchingExperimentStore& store,
    const StitchingExperimentCatalog& catalog,
    const QJsonObject& published,
    const QString& profile_id,
    int maximum,
    const QString& workflow_id) {
  try {
    if (QUuid(workflow_id).isNull() || maximum < 1 || maximum > 256)
      throw std::runtime_error("Invalid workflow identity or parallel stage limit");
    QJsonObject available;
    for (const auto& value : published.value("profiles").toArray()) {
      auto profile = value.toObject();
      const auto id = profile.take("id").toString();
      available.insert(id, profile);
    }
    const auto cpu_id = published.value("admission_profile").toString();
    if (!available.contains(profile_id) || !available.contains(cpu_id))
      throw std::runtime_error("The selected execution or metadata profile is unavailable");
    const auto gpu = available.value(profile_id).toObject();
    const auto cpu = available.value(cpu_id).toObject();
    if (cpu.value("resources").toObject().value("gpus").toInt(-1) != 0)
      throw std::runtime_error("Metadata preparation requires a CPU admission profile");
    QJsonObject profiles{{profile_id, gpu}, {cpu_id, cpu}};
    std::map<QString, const StoredStitchingExperiment*> rows;
    for (const auto& row : catalog.experiments)
      rows.emplace(keyFor(store, row), &row);
    QJsonArray stages;
    std::set<QString> added, selecting;
    QJsonObject identities;
    auto add = [&](const QString& key,
                   const QString& phase,
                   const QStringList& dependencies,
                   QJsonObject inputs,
                   bool metadata) {
      const auto id = phase + "-" + QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex().left(20);
      if (!added.insert(id).second)
        return id;
      const auto& row = *rows.at(key);
      const auto selected_profile = metadata ? cpu_id : profile_id;
      const auto profile = profiles.value(selected_profile).toObject();
      const auto operation = "experiment-" + phase;
      if (!profile.value("capabilities").toArray().contains(operation))
        throw std::runtime_error(("The resource profile does not support " + operation).toStdString());
      if ((phase == "baseline" || phase == "run" || phase == "materialize") && row.state == "queued" &&
          row.workflow_id.empty())
        inputs.insert("catalog_key", key);
      QJsonArray parents;
      for (const auto& dependency : dependencies)
        if (!parents.contains(dependency))
          parents.append(dependency);
      QString label = phase == "run"   ? "Calibrate · " + QString::number(row.workspace.settings.control_points) + " CP"
          : phase == "create"          ? "Set up experiment files (CPU)"
          : phase == "baseline"        ? "Initial calibration"
          : phase == "scan"            ? "Find player-rich frames"
          : phase == "materialize"     ? "Extract selected frames"
          : phase == "reuse-selection" ? "Use shared frames (CPU)"
                                       : phase;
      if (phase == "create" &&
          (!row.saved_selection_fingerprint.empty() ||
           (!row.selection_fingerprint.empty() &&
            (row.selection_workspace || row.result_workspace || row.state == "complete"))))
        label = "Set up retained frames (CPU)";
      if (!label.isEmpty())
        label[0] = label[0].toUpper();
      stages.append(
          QJsonObject{
              {"id", id},
              {"kind", operation},
              {"label", label + " · #" + QString::number(row.sequence)},
              {"candidate_id", key},
              {"dependencies", parents},
              {"inputs", inputs},
              {"profile_id", selected_profile}});
      return id;
    };
    auto prepare = [&](const QString& key) {
      const auto& row = *rows.at(key);
      identities.insert(
          key,
          QJsonObject{
              {"workspace", descriptor(ResolvedStitchingExperimentWorkspace(row))},
              {"revision", double(row.revision)},
              {"source_config_revision", text(row.source_config_revision)}});
      return add(key, "create", {}, {{"workspace", descriptor(ResolvedStitchingExperimentWorkspace(row))}}, true);
    };
    std::function<QString(const QString&)> baseline = [&](const QString& key) {
      const auto& row = *rows.at(key);
      if (row.state != "queued")
        throw std::runtime_error("The required baseline must be complete or queued");
      const auto prepared = prepare(key);
      return add(key, "baseline", {prepared}, {{"workspace", reference(prepared)}, {"require_ice_mask", true}}, false);
    };
    std::function<QString(const QString&)> selection = [&](const QString& key) {
      if (!rows.count(key))
        throw std::runtime_error("The saved selection owner is missing");
      if (!selecting.insert(key).second)
        throw std::runtime_error("The saved selection dependencies contain a cycle");
      const auto& owner = *rows.at(key);
      auto source = prepare(key);
      if (!owner.selection_fingerprint.empty() &&
          (owner.selection_workspace || owner.result_workspace || owner.state == "complete")) {
        selecting.erase(key);
        return source;
      }
      if (owner.selection_fingerprint.empty()) {
        const auto baseline_key = text(owner.baseline_workspace_key);
        if (!rows.count(baseline_key))
          throw std::runtime_error("A player scan has no saved baseline");
        const auto& saved = *rows.at(baseline_key);
        QJsonObject baseline_input;
        QStringList dependencies{source};
        if (saved.state == "complete") {
          baseline_input = descriptor(ResolvedStitchingExperimentWorkspace(saved));
          identities.insert(
              baseline_key,
              QJsonObject{
                  {"workspace", baseline_input},
                  {"revision", double(saved.revision)},
                  {"source_config_revision", text(saved.source_config_revision)}});
        } else {
          const auto solved = baseline(baseline_key);
          baseline_input = reference(solved);
          dependencies.append(solved);
        }
        source =
            add(key,
                "scan",
                dependencies,
                {{"workspace", reference(source)},
                 {"baseline_workspace", baseline_input},
                 {"search_duration_seconds", owner.scan_duration_seconds}},
                false);
      }
      const auto result = add(key, "materialize", {source}, {{"workspace", reference(source)}}, false);
      selecting.erase(key);
      return result;
    };
    for (const auto& [key, saved] : rows) {
      const auto& row = *saved;
      if (row.state != "queued" || !row.workflow_id.empty())
        continue;
      if (row.workflow_active || row.process_session_id || !row.process_token.empty())
        throw std::runtime_error("A queued candidate already has an active owner");
      if (row.requires_ice_mask && row.selection_owner_workspace_key.empty()) {
        baseline(key);
        continue;
      }
      auto source = prepare(key);
      if (!row.selection_owner_workspace_key.empty()) {
        const auto selected = selection(text(row.selection_owner_workspace_key));
        if (text(row.selection_owner_workspace_key) == key)
          source = selected;
        else
          source =
              add(key,
                  "reuse-selection",
                  {source, selected},
                  {{"workspace", reference(source)}, {"selection_workspace", reference(selected)}},
                  true);
      }
      add(key, "run", {source}, {{"workspace", reference(source)}, {"require_ice_mask", row.requires_ice_mask}}, false);
    }
    if (stages.isEmpty())
      throw std::runtime_error("No prepared candidates are queued");
    if (stages.size() > 256)
      throw std::runtime_error("This batch exceeds the 256-stage workflow limit");
    const auto user = hm::user_config::load_snapshot();
    if (!user.ok())
      return user.status();
    auto runtime_identity = gpu.value("runtime").toObject();
    runtime_identity.insert(
        "user_config_snapshot",
        QJsonObject{{"schema", 1}, {"source_path", text(user->path.string())}, {"values", jsonValue(user->values)}});
    const QJsonObject plan{
        {"schema", 1},
        {"workflow_id", workflow_id},
        {"kind", "stitching-experiments"},
        {"source_identity",
         QJsonObject{{"game_directory", text(store.game_directory.string())}, {"candidates", identities}}},
        {"configuration_digest", digest(identities)},
        {"runtime_identity", runtime_identity},
        {"profiles", profiles},
        {"limits", QJsonObject{{"max_parallel", maximum}}},
        {"stages", stages}};
    if (QJsonDocument(plan).toJson(QJsonDocument::Compact).size() > kMaxWorkflowDocumentBytes)
      throw std::runtime_error("Workflow plan exceeds the workflow byte limit");
    return plan;
  } catch (const std::exception& error) {
    return absl::FailedPreconditionError(error.what());
  }
}

struct StitchingWorkflow::Impl {
  StitchingWorkflow* owner;
  QString game, registry, launcher;
  QProcessEnvironment environment;
  QComboBox* profile{nullptr};
  QSpinBox* parallel{nullptr};
  QPushButton* export_batch{nullptr};
  QGroupBox* monitor{nullptr};
  QPointer<QDialog> monitor_dialog;
  QPushButton* expand_monitor{nullptr};
  QLabel* summary{nullptr};
  StageGraph* graph{nullptr};
  QComboBox* stage{nullptr};
  QComboBox* attempt{nullptr};
  QPlainTextEdit* details{nullptr};
  QPushButton* retry{nullptr};
  QPushButton* observer{nullptr};
  QTimer* poll{nullptr};
  QJsonObject profiles, snapshot;
  QString workflow, selected_candidate, transient_notice;
  bool command_pending{false}, status_pending{false}, profiles_ready{false}, editing{true}, queued{false};
  bool restore_controls{false};
  int timeout_ms{0};
  uint64_t request_generation{0};
  int retry_epoch{-1};
  bool log_pending{false};
  QPointer<QPlainTextEdit> log_view;
  void showMonitor(bool visible) {
    monitor->setVisible(visible);
    owner->setSizePolicy(
        QSizePolicy::Preferred, visible && !monitor_dialog ? QSizePolicy::Expanding : QSizePolicy::Maximum);
  }
  void expandMonitor() {
    if (monitor_dialog) {
      monitor_dialog->close();
      return;
    }
    auto* dialog = new QDialog(owner, Qt::Window | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint);
    dialog->setObjectName("stitchWorkflowExpandedWindow");
    dialog->setWindowTitle("Experiment workflow");
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setSizeGripEnabled(true);
    monitor_dialog = dialog;
    owner->layout()->removeWidget(monitor);
    auto* layout = new QVBoxLayout(dialog);
    layout->addWidget(monitor);
    expand_monitor->setIcon(action_icon(ActionIcon::Restore));
    expand_monitor->setToolTip("Restore workflow to the experiment window");
    expand_monitor->setAccessibleName("Restore workflow");
    owner->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    QObject::connect(dialog, &QDialog::finished, owner, [this, dialog] {
      dialog->layout()->removeWidget(monitor);
      owner->layout()->addWidget(monitor);
      monitor_dialog.clear();
      expand_monitor->setIcon(action_icon(ActionIcon::Expand));
      expand_monitor->setToolTip("Expand workflow");
      expand_monitor->setAccessibleName("Expand workflow");
      showMonitor(true);
    });
    dialog->resize(std::max(900, owner->window()->width()), std::max(650, owner->window()->height()));
    dialog->show();
  }
  QJsonObject selectedNode() const {
    for (const auto& value : snapshot.value("nodes").toArray())
      if (value.toObject().value("id").toString() == stage->currentData().toString())
        return value.toObject();
    return {};
  }
  void notify(const QString& value, bool error = false, const QString& replaces = {}) {
    transient_notice.clear();
    if (owner->message)
      owner->message(value, error, replaces);
  }
  void notifyUnconfirmed(const QString& value) {
    const auto previous = transient_notice;
    notify(value, true, previous);
    transient_notice = value;
  }
  bool currentSnapshot(const QJsonObject& value) const {
    const auto epoch = value.value("epoch");
    const auto revision = value.value("revision");
    const auto state = value.value("state").toString();
    return value.value("workflow_id").toString() == workflow && epoch.isDouble() && epoch.toInt(-1) >= 0 &&
        revision.isDouble() && revision.toInt(-1) >= 0 && value.value("nodes").isArray() &&
        (!value.contains("owner_released") || value.value("owner_released").isBool()) &&
        (state == "queued" || state == "running" || state == "cancelling" || state == "recovery" || terminal(state)) &&
        epoch.toInt() >= snapshot.value("epoch").toInt() && revision.toInt() >= snapshot.value("revision").toInt();
  }
  void resolveNotice() {
    if (transient_notice.isEmpty())
      return;
    const auto previous = transient_notice;
    const auto state = snapshot.value("state").toString();
    QString value = "Workflow confirmed: " + state + ".";
    bool error = state == "failed" || state == "recovery";
    if (terminal(state) && !snapshot.value("owner_released").toBool())
      value += " Waiting for resource cleanup.";
    else if (state == "succeeded")
      value += " Completed experiments are ready to preview or use in main Program.";
    else if (error)
      value += " Select the affected stage and open its log for details.";
    else if (!terminal(state))
      value = "Workflow status recovered. Follow current progress in the Workflow tab.";
    for (const auto* key : {"controller_error", "cancel_error", "controller_warning"}) {
      const auto detail = snapshot.value(key).toObject().value("error").toString();
      if (!detail.isEmpty()) {
        value += " " + detail;
        error = true;
      }
    }
    notify(value, error, previous);
    if ((terminal(state) && !snapshot.value("owner_released").toBool()) ||
        snapshot.contains("controller_error") || snapshot.contains("cancel_error") ||
        snapshot.contains("controller_warning"))
      transient_notice = value;
  }
  void call(QStringList arguments, std::function<void(QJsonObject, QString)> done) {
    if (launcher.isEmpty() || registry.isEmpty()) {
      done({}, "Workflow registry or controller command is unavailable");
      return;
    }
    arguments << "--registry" << registry;
    auto* process = new QProcess(owner);
    process->setProcessEnvironment(environment);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    auto output = std::make_shared<QByteArray>();
    auto error = std::make_shared<QByteArray>();
    auto termination = std::make_shared<QString>();
    auto finished = std::make_shared<bool>(false);
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    QObject::connect(
        timer, &QTimer::timeout, process, [process, termination, operation = arguments.front(), timeout = timeout_ms] {
          *termination = QString("Workflow %1 command timed out after %2 seconds.").arg(operation).arg(timeout / 1000);
          process->kill();
        });
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, [process, output, termination] {
      output->append(process->readAllStandardOutput());
      if (output->size() > kMaxWorkflowDocumentBytes) {
        *termination = "Workflow response exceeded its size limit.";
        process->kill();
      }
    });
    QObject::connect(process, &QProcess::readyReadStandardError, process, [process, error, termination] {
      error->append(process->readAllStandardError());
      if (error->size() > 1024 * 1024) {
        *termination = "Workflow error response exceeded its size limit.";
        process->kill();
      }
    });
    auto finish = [process, output, error, termination, done = std::move(done), finished, timer](
                      int code, QProcess::ExitStatus exit_status) {
      if (*finished)
        return;
      *finished = true;
      timer->stop();
      output->append(process->readAllStandardOutput());
      error->append(process->readAllStandardError());
      QJsonParseError parse;
      const auto document = QJsonDocument::fromJson(*output, &parse);
      QString failure = *termination;
      if (failure.isEmpty() &&
          (code != 0 || exit_status != QProcess::NormalExit || parse.error != QJsonParseError::NoError ||
           !document.isObject() || output->size() > kMaxWorkflowDocumentBytes)) {
        const auto error_document = QJsonDocument::fromJson(*error);
        failure = error_document.object().value("error").toString();
        if (failure.isEmpty())
          failure = QString::fromUtf8(error->left(8192));
        if (failure.isEmpty()) {
          if (process->error() == QProcess::FailedToStart)
            failure = "Could not start the workflow command: " + process->errorString();
          else if (exit_status == QProcess::CrashExit)
            failure = "The workflow command stopped unexpectedly before returning a response.";
          else if (code != 0)
            failure = QString("The workflow command exited with code %1 without an error message.").arg(code);
          else if (output->trimmed().isEmpty())
            failure = "The workflow command returned an empty response.";
          else
            failure = "The workflow command returned an invalid response.";
        }
      }
      auto value = document.object();
      if (!failure.isEmpty())
        value = QJsonDocument::fromJson(*error).object();
      if (process->error() == QProcess::FailedToStart)
        value.insert("client_not_started", true);
      if (!failure.isEmpty())
        qWarning().noquote() << "Workflow command failed:" << process->program() << process->arguments().value(0)
                             << "exit=" << code << "status=" << exit_status << "stdout_bytes=" << output->size()
                             << "stderr_bytes=" << error->size() << failure;
      done(value, failure);
      process->deleteLater();
    };
    QObject::connect(
        process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        owner,
        [finish](int code, QProcess::ExitStatus status) { finish(code, status); });
    QObject::connect(process, &QProcess::errorOccurred, owner, [finish](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart)
        finish(-1, QProcess::NormalExit);
    });
    process->start(launcher, arguments);
    timer->start(timeout_ms);
  }
  void controls() {
    profile->setEnabled(editing && !command_pending && !owner->active() && profiles_ready);
    parallel->setEnabled(profile->isEnabled() && owner->selected());
    export_batch->setEnabled(
        editing && queued && owner->selected() && profiles_ready && !command_pending && !owner->active());
    const auto node = selectedNode();
    retry->setEnabled(
        editing && !command_pending && !owner->active() && !workflow.isEmpty() &&
        terminal(snapshot.value("state").toString()) && unsuccessful(node.value("state").toString()));
  }
  void renderDetails() {
    const auto node = selectedNode();
    const auto selected_attempt = attempt->currentData().toString();
    QJsonObject saved;
    for (const auto& value : node.value("attempts").toArray())
      if (value.toObject().value("id").toString() == selected_attempt)
        saved = value.toObject();
    const auto fields = saved.value("details").toObject();
    QString value = stageLabel(node) + "\n" + stageDescription(node, snapshot.value("nodes").toArray()) +
        "\nState: " + node.value("state").toString() + "\n" + node.value("reason").toString() +
        "\nStage ID: " + node.value("id").toString();
    if (!saved.isEmpty()) {
      value += "\nAttempt: " + saved.value("id").toString() + " (epoch " +
          QString::number(saved.value("epoch").toInt()) + ")";
      for (const auto& item :
           {std::pair<QString, QString>{"scheduler_id", "Slurm job"},
            {"host", "Host"},
            {"cluster", "Cluster"},
            {"reason", "Queue / task reason"}})
        if (!fields.value(item.first).toString().isEmpty())
          value += "\n" + item.second + ": " + fields.value(item.first).toString();
    }
    if (node.contains("resources"))
      value += "\nResources: " +
          QString::fromUtf8(QJsonDocument(node.value("resources").toObject()).toJson(QJsonDocument::Compact));
    for (const auto& key : {"controller_error", "controller_warning", "observer_error", "cancel_error"}) {
      const auto error = snapshot.value(key).toObject().value("error").toString();
      if (!error.isEmpty())
        value += "\n" + QString::fromUtf8(key) + ": " + error;
    }
    details->setPlainText(value);
    controls();
  }
  void chooseStage() {
    const auto previous = attempt->currentData().toString();
    QSignalBlocker blocker(attempt);
    attempt->clear();
    const auto node = selectedNode();
    for (const auto& value : node.value("attempts").toArray()) {
      const auto saved = value.toObject();
      attempt->addItem(
          QString("Epoch %1 · %2 · %3")
              .arg(saved.value("epoch").toInt())
              .arg(saved.value("state").toString(), saved.value("id").toString().left(8)),
          saved.value("id").toString());
    }
    const int old = attempt->findData(previous);
    attempt->setCurrentIndex(old >= 0 ? old : attempt->count() - 1);
    graph->showStages(snapshot.value("nodes").toArray(), stage->currentData().toString());
    renderDetails();
    if (log_view)
      loadLog();
    if (owner->changed)
      owner->changed();
  }
  void restoreAttachedControls() {
    if (!restore_controls || !profiles_ready || snapshot.value("nodes").toArray().isEmpty())
      return;
    QString profile_id;
    for (const auto& value : snapshot.value("nodes").toArray()) {
      const auto node = value.toObject();
      if (node.value("kind").toString() == "experiment-run") {
        profile_id = node.value("profile_id").toString();
        break;
      }
    }
    const int maximum = snapshot.value("limits").toObject().value("max_parallel").toInt();
    if (profile_id.isEmpty() || maximum < 1 || maximum > 256)
      return;
    QSignalBlocker profile_block(profile), parallel_block(parallel);
    int index = profile->findData(profile_id);
    if (index < 0) {
      profile->addItem(profile_id + " (retained profile unavailable)", profile_id);
      index = profile->count() - 1;
    }
    profile->setCurrentIndex(index);
    parallel->setValue(maximum);
    restore_controls = false;
  }
  void render(QJsonObject value) {
    if (!currentSnapshot(value))
      return;
    snapshot = std::move(value);
    restoreAttachedControls();
    const auto current = stage->currentData().toString();
    {
      QSignalBlocker blocker(stage);
      stage->clear();
      for (const auto& node : snapshot.value("nodes").toArray()) {
        const auto row = node.toObject();
        stage->addItem(stageLabel(row) + " · " + row.value("state").toString(), row.value("id").toString());
      }
      int index = stage->findData(current);
      if (index < 0 && !selected_candidate.isEmpty()) {
        for (const auto& value : snapshot.value("nodes").toArray()) {
          const auto node = value.toObject();
          if (node.value("candidate_id").toString() != selected_candidate)
            continue;
          index = stage->findData(node.value("id").toString());
          const auto kind = node.value("kind").toString();
          if (kind == "experiment-run" || kind == "experiment-baseline")
            break;
        }
      }
      if (index < 0 && selected_candidate.isEmpty() && stage->count() > 0)
        index = 0;
      stage->setCurrentIndex(index);
    }
    showMonitor(true);
    summary->setText(
        "Workflow " + workflow + " · " + snapshot.value("state").toString() +
        (terminal(snapshot.value("state").toString()) && !snapshot.value("owner_released").toBool()
             ? " · waiting for game ownership release"
             : ""));
    const auto observer_value = snapshot.value("observer").toObject();
    const auto url = QUrl(observer_value.value("url").toString());
    observer->setVisible(
        (url.scheme() == "http" || url.scheme() == "https") && !observer_value.value("run_id").toString().isEmpty());
    observer->setProperty("url", url);
    chooseStage();
    if (owner->snapshotChanged)
      owner->snapshotChanged(snapshot);
    if (owner->changed)
      owner->changed();
    resolveNotice();
  }
  void refresh() {
    if (workflow.isEmpty() || status_pending || command_pending)
      return;
    status_pending = true;
    const auto generation = request_generation;
    call({"status", "--workflow", workflow}, [this, generation](QJsonObject result, QString error) {
      status_pending = false;
      if (generation != request_generation)
        return;
      if (!error.isEmpty()) {
        summary->setText("Workflow " + workflow + " · monitoring unavailable: " + error);
        notifyUnconfirmed("Workflow status is temporarily unavailable: " + error + " Checking again automatically.");
        return;
      }
      if (!currentSnapshot(result)) {
        notifyUnconfirmed("Waiting for current status of this workflow. Do not start the batch again.");
        return;
      }
      if (retry_epoch >= 0) {
        if (result.value("workflow_id").toString() != workflow || !result.value("epoch").isDouble() ||
            result.value("epoch").toInt(-1) < retry_epoch) {
          notifyUnconfirmed("Workflow retry admission is still unresolved; waiting for current status.");
          return;
        }
        // The launcher execs the installed controller in the same PID. Retry
        // writes its epoch synchronously before launching any helpers. This
        // read began after that process exited; an unchanged released epoch
        // therefore proves rejection, while a new epoch proves acceptance.
        retry_epoch = -1;
      }
      render(std::move(result));
    });
  }
  void loadLog() {
    if (!log_view)
      return;
    if (!owner->hasSelection() || attempt->currentData().toString().isEmpty()) {
      log_view->setPlainText("No attempt log is available. " + selectedNode().value("reason").toString());
      return;
    }
    if (log_pending)
      return;
    log_pending = true;
    const auto selected_stage = stage->currentData().toString();
    const auto selected_attempt = attempt->currentData().toString();
    call(
        {"log", "--workflow", workflow, "--stage", selected_stage, "--attempt", selected_attempt},
        [this, selected_stage, selected_attempt](QJsonObject value, QString error) {
          log_pending = false;
          if (!log_view || stage->currentData().toString() != selected_stage ||
              attempt->currentData().toString() != selected_attempt)
            return;
          log_view->setPlainText(error.isEmpty() ? value.value("text").toString() : error);
        });
  }
  void submitPlan(const StitchingExperimentStore& store, const StitchingExperimentCatalog& catalog, bool exporting) {
    if (command_pending || !profiles_ready || !owner->selected())
      return;
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto plan =
        BuildStitchingWorkflowPlan(store, catalog, profiles, profile->currentData().toString(), parallel->value(), id);
    if (!plan.ok()) {
      notify(text(plan.status().ToString()), true);
      return;
    }
    QString destination;
    if (exporting) {
      destination = QFileDialog::getSaveFileName(
          owner, "Export stitching batch", QDir::homePath() + "/stitching-batch.sh", "Shell scripts (*.sh)");
      if (destination.isEmpty())
        return;
    }
    auto temporary = std::make_shared<QTemporaryFile>(QDir::tempPath() + "/hstream-workflow-XXXXXX.json");
    if (!temporary->open() || temporary->write(QJsonDocument(*plan).toJson(QJsonDocument::Compact)) < 0 ||
        !temporary->flush()) {
      notify("Could not save the private workflow request", true);
      return;
    }
    temporary->close();
    command_pending = true;
    transient_notice.clear();
    ++request_generation;
    if (!exporting) {
      workflow = id;
      snapshot = {{"workflow_id", id}, {"state", "submitting"}, {"epoch", 0}, {"nodes", QJsonArray{}}};
      showMonitor(true);
      summary->setText("Submitting workflow " + id);
      // The controller can accept the request before its response is lost. Keep
      // observing this exact ID even when submission or the first status fails.
      poll->start();
    }
    controls();
    if (owner->changed)
      owner->changed();
    QStringList arguments{exporting ? "export" : "submit", "--plan", temporary->fileName()};
    if (exporting)
      arguments << "--output" << destination;
    else
      arguments << "--game-dir" << game;
    call(arguments, [this, temporary, exporting, id](QJsonObject result, QString error) {
      command_pending = false;
      if (!error.isEmpty()) {
        if (exporting)
          notify(error, true);
        else {
          if (result.value("client_not_started").toBool() ||
              (result.value("submission").toString() == "rejected" && result.value("workflow_id").toString() == id)) {
            notify(error, true);
            poll->stop();
            workflow.clear();
            snapshot = {};
            showMonitor(false);
          } else {
            notifyUnconfirmed(error + " Checking whether the batch started. Do not start it again.");
            refresh();
          }
        }
      } else if (exporting)
        notify("Exported batch: " + result.value("script").toString());
      else {
        if (currentSnapshot(result)) {
          render(result);
          notify("Workflow submitted. Closing this dialog requires cancellation and resource cleanup.");
        } else {
          notifyUnconfirmed(
              "Submission returned without current workflow status. Checking whether the batch started. Do not start it again.");
          refresh();
        }
      }
      controls();
      if (owner->changed)
        owner->changed();
    });
  }
};

StitchingWorkflow::StitchingWorkflow(
    const QString& game,
    const QString& working_directory,
    const QProcessEnvironment& environment,
    const YAML::Node& settings,
    QWidget* parent)
    : QWidget(parent), impl_(std::make_unique<Impl>()) {
  auto& s = *impl_;
  s.owner = this;
  s.game = game;
  s.environment = environment;
  setObjectName("stitchingWorkflowPane");
  setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
  const auto configuration = settings["workflow"];
  if (!configuration || !configuration.IsMap() || !configuration["max_parallel"] ||
      !configuration["poll_interval_ms"] || !configuration["command_timeout_seconds"])
    throw std::runtime_error("Workflow defaults are missing from the baseline");
  if (configuration["registry"] && !configuration["registry"].IsNull())
    s.registry = text(configuration["registry"].as<std::string>());
  if (!s.registry.isEmpty() && !QFileInfo(s.registry).isAbsolute())
    throw std::runtime_error("Workflow registry must be an absolute path");
  s.timeout_ms = configuration["command_timeout_seconds"].as<int>() * 1000;
  if (s.timeout_ms <= 0)
    throw std::runtime_error("Invalid workflow command timeout");
  const QString source = QDir(working_directory).filePath("scripts/hstream-workflow");
  s.launcher = QFileInfo(source).isExecutable() ? source : QStandardPaths::findExecutable("hstream-workflow");
  auto* layout = new QVBoxLayout(this);
  layout->setContentsMargins(0, 0, 0, 0);
  auto* controls = new QFormLayout();
  s.profile = new QComboBox(this);
  s.profile->setObjectName("stitchWorkflowProfile");
  s.profile->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  s.profile->setMinimumContentsLength(12);
  s.profile->addItem("Current runner", "");
  s.parallel = new QSpinBox(this);
  s.parallel->setObjectName("stitchWorkflowParallel");
  s.parallel->setRange(1, 256);
  s.parallel->setValue(configuration["max_parallel"].as<int>());
  s.export_batch = new QPushButton(action_icon(ActionIcon::Save), "Export batch", this);
  s.export_batch->setObjectName("exportStitchWorkflowButton");
  controls->addRow("Execution profile", s.profile);
  auto* parallel_row = new QHBoxLayout();
  parallel_row->addWidget(s.parallel);
  parallel_row->addWidget(s.export_batch);
  controls->addRow("Parallel stages", parallel_row);
  layout->addLayout(controls);
  s.monitor = new QGroupBox("Workflow", this);
  s.monitor->setObjectName("stitchWorkflowMonitor");
  auto* monitor_outer = new QVBoxLayout(s.monitor);
  auto* monitor_scroll = new QScrollArea(s.monitor);
  monitor_scroll->setObjectName("stitchWorkflowMonitorScroll");
  monitor_scroll->setWidgetResizable(true);
  monitor_scroll->setMinimumHeight(100);
  auto* monitor_content = new QWidget(monitor_scroll);
  auto* monitor = new QVBoxLayout(monitor_content);
  monitor_scroll->setWidget(monitor_content);
  monitor_outer->addWidget(monitor_scroll);
  s.summary = new QLabel(s.monitor);
  s.summary->setWordWrap(true);
  s.summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
  auto* summary_row = new QHBoxLayout();
  summary_row->addWidget(s.summary, 1);
  s.expand_monitor = new QPushButton(action_icon(ActionIcon::Expand), "", s.monitor);
  s.expand_monitor->setObjectName("expandStitchWorkflowButton");
  s.expand_monitor->setToolTip("Expand workflow");
  s.expand_monitor->setAccessibleName("Expand workflow");
  summary_row->addWidget(s.expand_monitor);
  monitor->addLayout(summary_row);
  connect(s.expand_monitor, &QPushButton::clicked, this, [&s] { s.expandMonitor(); });
  auto* scroll = new QScrollArea(s.monitor);
  scroll->setObjectName("stitchWorkflowGraphScroll");
  scroll->setWidgetResizable(true);
  scroll->setMinimumHeight(100);
  s.graph = new StageGraph(scroll);
  s.graph->setToolTip("Arrows show prerequisites. Shared frame steps run once for matching experiments.");
  scroll->setWidget(s.graph);
  monitor->addWidget(scroll, 1);
  auto* selectors = new QFormLayout();
  s.stage = new QComboBox(s.monitor);
  s.stage->setObjectName("stitchWorkflowStage");
  s.stage->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
  s.stage->setMinimumContentsLength(12);
  s.attempt = new QComboBox(s.monitor);
  s.attempt->setObjectName("stitchWorkflowAttempt");
  selectors->addRow("Stage", s.stage);
  selectors->addRow("Attempt", s.attempt);
  monitor->addLayout(selectors);
  s.details = new QPlainTextEdit(s.monitor);
  s.details->setObjectName("stitchWorkflowDetails");
  s.details->setReadOnly(true);
  s.details->setMaximumHeight(105);
  monitor->addWidget(s.details);
  auto* actions = new QHBoxLayout();
  s.retry = new QPushButton(action_icon(ActionIcon::Refresh), "Retry selected stage", s.monitor);
  s.retry->setObjectName("retryStitchWorkflowStageButton");
  s.observer = new QPushButton(action_icon(ActionIcon::Inspect), "Open in Dagster", s.monitor);
  s.observer->setObjectName("openStitchWorkflowDagsterButton");
  s.observer->hide();
  actions->addWidget(s.retry);
  actions->addWidget(s.observer);
  monitor->addLayout(actions);
  layout->addWidget(s.monitor, 1);
  s.monitor->hide();
  s.poll = new QTimer(this);
  s.poll->setInterval(configuration["poll_interval_ms"].as<int>());
  if (s.poll->interval() < 1)
    throw std::runtime_error("Invalid workflow poll interval");
  connect(s.poll, &QTimer::timeout, this, [&s] { s.refresh(); });
  connect(s.profile, qOverload<int>(&QComboBox::currentIndexChanged), this, [&s] {
    s.controls();
    if (s.owner->changed)
      s.owner->changed();
  });
  connect(s.export_batch, &QPushButton::clicked, this, [this] {
    if (exportRequested)
      exportRequested();
  });
  connect(s.stage, qOverload<int>(&QComboBox::currentIndexChanged), this, [&s] { s.chooseStage(); });
  connect(s.attempt, qOverload<int>(&QComboBox::currentIndexChanged), this, [&s] {
    s.renderDetails();
    if (s.log_view)
      s.loadLog();
  });
  s.graph->selected = [&s](const QString& id) { s.stage->setCurrentIndex(s.stage->findData(id)); };
  connect(s.observer, &QPushButton::clicked, this, [&s] {
    QDesktopServices::openUrl(s.observer->property("url").toUrl());
  });
  connect(s.retry, &QPushButton::clicked, this, [&s] {
    if (!s.retry->isEnabled())
      return;
    s.command_pending = true;
    ++s.request_generation;
    s.retry_epoch = s.snapshot.value("epoch").toInt();
    s.controls();
    if (s.owner->changed)
      s.owner->changed();
    s.call(
        {"retry",
         "--workflow",
         s.workflow,
         "--stage",
         s.stage->currentData().toString(),
         "--epoch",
         QString::number(s.snapshot.value("epoch").toInt())},
        [&s](QJsonObject value, QString error) {
          s.command_pending = false;
          if (!error.isEmpty()) {
            if (value.value("client_not_started").toBool())
              s.retry_epoch = -1;
            if (s.retry_epoch >= 0) {
              s.notifyUnconfirmed(error + " Checking the retained workflow before retrying again.");
              s.refresh();
            } else
              s.notify(error, true);
          } else if (
              s.currentSnapshot(value) && value.value("epoch").toInt(-1) > s.retry_epoch) {
            s.retry_epoch = -1;
            s.render(value);
          } else
            s.refresh();
          s.controls();
          if (s.owner->changed)
            s.owner->changed();
        });
  });
  s.controls();
  if (s.registry.isEmpty())
    s.profile->setToolTip("Configure workflow.registry to enable managed local and Slurm profiles.");
  else
    QTimer::singleShot(0, this, [&s] {
      s.call({"profiles"}, [&s](QJsonObject value, QString error) {
        if (!error.isEmpty()) {
          s.notify(error, true);
          s.profile->setToolTip(error);
          return;
        }
        s.profiles = value;
        for (const auto& profile : value.value("profiles").toArray()) {
          const auto object = profile.toObject();
          const auto capabilities = object.value("capabilities").toArray();
          if (!capabilities.contains("experiment-run"))
            continue;
          const auto id = object.value("id").toString();
          const auto label = object.value("label").toString();
          s.profile->addItem((label.isEmpty() ? id : label) + " (" + object.value("executor").toString() + ")", id);
        }
        s.profiles_ready = true;
        s.restoreAttachedControls();
        s.controls();
        if (s.owner->changed)
          s.owner->changed();
      });
    });
}
StitchingWorkflow::~StitchingWorkflow() {
  impl_->poll->stop();
  // These are short request clients, never compute supervisors. Disconnect their
  // callbacks before Impl is destroyed; detaching does not issue cancellation.
  for (auto* process : findChildren<QProcess*>()) {
    process->setParent(nullptr);
    QObject::disconnect(process, nullptr, nullptr, nullptr);
    if (process->state() != QProcess::NotRunning) {
      QObject::connect(
          process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process, &QObject::deleteLater);
      process->kill();
    } else
      process->deleteLater();
  }
}
bool StitchingWorkflow::selected() const {
  return !impl_->profile->currentData().toString().isEmpty();
}
void StitchingWorkflow::restoreMonitor() {
  if (impl_->monitor_dialog)
    impl_->monitor_dialog->close();
}
bool StitchingWorkflow::active() const {
  return !impl_->workflow.isEmpty() &&
      (impl_->retry_epoch >= 0 || !terminal(impl_->snapshot.value("state").toString()) ||
       !impl_->snapshot.value("owner_released").toBool());
}
bool StitchingWorkflow::busy() const {
  return impl_->command_pending || impl_->retry_epoch >= 0;
}
bool StitchingWorkflow::hasSelection() const {
  return !impl_->selectedNode().isEmpty();
}
QString StitchingWorkflow::workflowId() const {
  return impl_->workflow;
}
void StitchingWorkflow::setEditingEnabled(bool enabled, bool queued) {
  impl_->editing = enabled;
  impl_->queued = queued;
  impl_->controls();
}
void StitchingWorkflow::start(const StitchingExperimentStore& store, const StitchingExperimentCatalog& catalog) {
  impl_->submitPlan(store, catalog, false);
}
void StitchingWorkflow::exportBatch(const StitchingExperimentStore& store, const StitchingExperimentCatalog& catalog) {
  impl_->submitPlan(store, catalog, true);
}
void StitchingWorkflow::cancel(std::function<void(bool)> completed) {
  auto& s = *impl_;
  if (!active() || busy())
    return;
  s.command_pending = true;
  ++s.request_generation;
  s.controls();
  s.call(
      {"cancel", "--workflow", s.workflow, "--epoch", QString::number(s.snapshot.value("epoch").toInt())},
      [&s, completed = std::move(completed)](QJsonObject value, QString error) {
        s.command_pending = false;
        if (!error.isEmpty())
          s.notify(error, true);
        else
          s.render(value);
        s.controls();
        if (completed)
          completed(error.isEmpty());
        if (s.owner->changed)
          s.owner->changed();
      });
}
void StitchingWorkflow::attach(const QString& id) {
  ++impl_->request_generation;
  impl_->transient_notice.clear();
  impl_->workflow = id;
  impl_->snapshot = {};
  impl_->restore_controls = true;
  impl_->showMonitor(true);
  impl_->poll->start();
  impl_->refresh();
}
void StitchingWorkflow::selectCandidate(const QString& key) {
  auto& s = *impl_;
  if (key == s.selected_candidate)
    return;
  s.selected_candidate = key;
  QString selected;
  for (const auto& value : s.snapshot.value("nodes").toArray()) {
    const auto node = value.toObject();
    if (node.value("candidate_id").toString() == key) {
      selected = node.value("id").toString();
      if (node.value("kind").toString() == "experiment-run" || node.value("kind").toString() == "experiment-baseline")
        break;
    }
  }
  s.stage->setCurrentIndex(s.stage->findData(selected));
}
void StitchingWorkflow::showLog() {
  auto& s = *impl_;
  if (!hasSelection())
    return;
  if (s.log_view) {
    s.log_view->window()->raise();
    s.loadLog();
    return;
  }
  auto* dialog = new QDialog(this);
  dialog->setAttribute(Qt::WA_DeleteOnClose);
  dialog->setWindowTitle("Workflow stage log");
  dialog->resize(850, 500);
  auto* layout = new QVBoxLayout(dialog);
  auto* log = new QPlainTextEdit(dialog);
  log->setObjectName("stitchWorkflowLog");
  log->setReadOnly(true);
  log->setMaximumBlockCount(20000);
  layout->addWidget(log);
  auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close", dialog);
  layout->addWidget(close);
  connect(close, &QPushButton::clicked, dialog, &QDialog::close);
  s.log_view = log;
  dialog->show();
  s.loadLog();
}
