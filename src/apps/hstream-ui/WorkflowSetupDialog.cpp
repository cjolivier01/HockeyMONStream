#include "src/apps/hstream-ui/WorkflowSetupDialog.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QTableWidget>
#include <QtWidgets/QVBoxLayout>
#include <functional>
#include "hstream/src/libs/common/UserConfig.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/LogTextEdit.h"

namespace {
QJsonValue jsonValue(const YAML::Node& value) {
  if (!value || value.IsNull())
    return QJsonValue::Null;
  if (value.IsMap()) {
    QJsonObject result;
    for (const auto& child : value)
      result.insert(QString::fromStdString(child.first.as<std::string>()), jsonValue(child.second));
    return result;
  }
  if (value.IsSequence()) {
    QJsonArray result;
    for (const auto& child : value)
      result.append(jsonValue(child));
    return result;
  }
  return QString::fromStdString(value.as<std::string>());
}
QString readableError(QString message) {
  for (int depth = 0; depth < 4; ++depth) {
    const auto error = QJsonDocument::fromJson(message.toUtf8()).object().value("error");
    if (!error.isString())
      break;
    message = error.toString();
  }
  return message.trimmed();
}
} // namespace

struct WorkflowSetupDialog::Impl {
  WorkflowSetupDialog* owner;
  QString launcher, accepted_registry;
  QString installed_runtime, installed_server;
  QProcessEnvironment environment;
  YAML::Node settings;
  QLineEdit *server, *registry, *controller_runtime, *dashboard, *sample;
  QPlainTextEdit* recordings;
  QComboBox* computers;
  QTableWidget* profiles;
  LogTextEdit* results;
  QLabel* next_step;
  QPushButton *discover, *reload, *save, *check, *use, *add, *remove, *use_installed;
  QPushButton *build, *test, *resume, *stop, *apply, *start, *stop_service;
  QJsonValue revision{QJsonValue::Null};
  QJsonObject retained_profiles;
  QJsonObject saved_draft, progress;
  bool tests_known{false}, tests_passed{false}, test_running{false};
  int controller_state{0}; // Unknown, running, or stopped.
  QJsonValue observer{QJsonValue::Null};
  QPointer<QProcess> process;
  int timeout;
  bool loading{false};
  bool polling{false};
  QTimer* poll;
  QJsonArray tests;

  void updateButtons() {
    if (loading)
      return;
    const bool idle = !process;
    const bool clean = !saved_draft.isEmpty() && saved_draft == draft();
    const bool current = clean && progress.value("revision") == revision;
    const bool built = progress.value("has_build").toBool();
    const bool build_current = current && progress.value("build_current").toBool();
    const bool applied = current && progress.value("applied").toBool();
    const bool saved_server = revision.isString() && !saved_draft.value("server").toString().isEmpty();
    const bool folder = QDir::isAbsolutePath(registry->text().trimmed());
    const bool server_set = !server->text().trimmed().isEmpty();
    const bool choices = server_set && folder && profiles->rowCount() > 0 &&
        !recordings->toPlainText().trimmed().isEmpty() && QDir::isAbsolutePath(controller_runtime->text().trimmed());
    const auto enable = [idle](QPushButton* button, bool available, const QString& reason) {
      button->setEnabled(idle && available);
      button->setToolTip(available ? QString{} : reason);
    };
    enable(discover, server_set, "Enter your main server name first.");
    enable(reload, folder, "Choose the shared settings folder first.");
    enable(
        save,
        folder && QDir::isAbsolutePath(controller_runtime->text().trimmed()),
        "Choose the settings and HStream installation folders first.");
    enable(add, profiles->rowCount() < 64, "At most 64 profiles can be saved.");
    enable(remove, profiles->currentRow() >= 0, "Select a profile row to remove.");
    enable(
        build,
        choices && (!built || (tests_known && !test_running)),
        built && (!tests_known || test_running)
            ? "Resume / check tests, and stop any unfinished test before rebuilding."
            : "Enter the server and recording folder, and add at least one computer profile.");
    enable(
        test,
        build_current && tests_known && !tests_passed && !test_running &&
            QDir::isAbsolutePath(sample->text().trimmed()),
        !build_current     ? "Save and build the current choices first."
            : !tests_known ? "Resume / check tests first."
            : test_running ? "Wait for the unfinished test, or stop it."
            : tests_passed ? "Every selected computer has passed. Apply tested settings next."
                           : "Choose the full folder path of a sample recording first.");
    enable(resume, built && saved_server, "Save the main server choice and build the registry first.");
    enable(
        stop,
        built && saved_server && (!tests_known || test_running),
        !saved_server ? "Save the main server choice first." : "There is no unfinished test to stop.");
    enable(
        apply,
        build_current && tests_known && tests_passed && !test_running && !applied && controller_state != 1,
        applied              ? "These tested settings are already applied."
            : !build_current ? "Save and build the current choices first."
            : !tests_known   ? "Resume / check tests first."
            : !tests_passed  ? "Every selected computer must pass its test first."
                             : "Stop the controller before applying tested settings.");
    enable(
        start,
        applied && controller_state != 1,
        "Apply tested settings first. If already started, check the installed setup.");
    enable(
        stop_service,
        saved_server && progress.value("installed").toBool() && controller_state != -1,
        !saved_server ? "Save the main server choice first."
                      : "No installed controller is known to need stopping. Recover or check an existing setup first.");
    enable(check, folder, "Choose the shared settings folder first.");
    enable(use, !accepted_registry.isEmpty(), "Check installed setup successfully before using the cluster.");
    use_installed->setEnabled(idle && !installed_runtime.isEmpty() && installed_server == server->text().trimmed());
    QString next;
    if (!idle)
      next = "Working… wait for this step to finish.";
    else if (!server_set)
      next = "Enter your main server, then Find computers / test connection. You can also Recover saved setup.";
    else if (!accepted_registry.isEmpty())
      next = "Setup is ready. Use this cluster to return to your experiment.";
    else if (built && !saved_server)
      next = "Save setup draft to record the main server, then Resume / check tests.";
    else if (built && !tests_known)
      next = "Resume / check tests to recover the saved test progress.";
    else if (test_running)
      next = "A test is unfinished. Keep this window open to follow it, or Stop test.";
    else if (!choices)
      next = "Find computers and add a profile. Check the recording and installation folders.";
    else if (!build_current)
      next = "Save and build registry to check these choices.";
    else if (!tests_passed)
      next = "Choose a sample recording, then Test next computer. Repeat until every selected computer passes.";
    else if (!applied && controller_state == 1)
      next = "Stop controller after existing jobs finish, then Apply tested settings.";
    else if (!applied)
      next = "Apply tested settings to install the tested profiles.";
    else if (controller_state == 1)
      next = "Check installed setup to verify the controller is ready.";
    else
      next = "Start controller, then Check installed setup.";
    next_step->setText("Next: " + next);
  }
  void remember(const QJsonObject& value, const QString& action) {
    if (action == "load" || action == "import") {
      tests_known = false;
      controller_state = 0;
    }
    if (value.value("draft").isObject())
      saved_draft = value.value("draft").toObject();
    if (value.contains("revision"))
      revision = value.value("revision");
    if (value.value("progress").isObject())
      progress = value.value("progress").toObject();
    if (action == "build") {
      tests_known = true;
      tests_passed = false;
      test_running = false;
    }
    if (value.contains("tests")) {
      tests_known = true;
      tests_passed = value.value("tested").toBool();
      test_running = false;
      for (const auto& item : value.value("tests").toArray()) {
        const auto row = item.toObject();
        test_running |= row.contains("status") && !row.value("finished").toBool();
      }
    }
    if (action == "start-service" || value.value("ready").toBool())
      controller_state = 1;
    if (action == "stop-service")
      controller_state = -1;
  }
  void installedRuntime(const QString& path = {}) {
    installed_runtime = path;
    installed_server = server->text().trimmed();
    use_installed->setEnabled(!path.isEmpty());
    use_installed->setToolTip(path.isEmpty() ? "Find computers / test connection first." : path);
  }
  void busy(bool value) {
    for (auto* button :
         {discover, reload, save, check, add, remove, build, test, resume, stop, apply, start, stop_service})
      button->setEnabled(!value);
    use_installed->setEnabled(!value && !installed_runtime.isEmpty() && installed_server == server->text().trimmed());
    server->setEnabled(!value);
    registry->setEnabled(!value);
    recordings->setEnabled(!value);
    profiles->setEnabled(!value);
    for (auto* control : {controller_runtime, dashboard, sample})
      control->setEnabled(!value);
    computers->setEnabled(!value);
    if (value)
      use->setEnabled(false);
    updateButtons();
  }
  void message(const QString& text, const QString& source = "setup") {
    results->setLogText(text, source);
  }
  void invalidate() {
    if (!loading) {
      use->setEnabled(false);
      accepted_registry.clear();
      updateButtons();
    }
  }
  void call(QJsonObject request, std::function<void(QJsonObject)> done) {
    if (process)
      return;
    accepted_registry.clear();
    const auto action = request.value("action").toString();
    if (action == "inspect" || action == "start-service" || action == "stop-service")
      controller_state = 0;
    auto temporary = std::make_shared<QTemporaryFile>();
    if (!temporary->open()) {
      message("Could not create the setup request: " + temporary->errorString());
      return;
    }
    temporary->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    temporary->close();
    auto* child = new QProcess(owner);
    process = child;
    child->setProcessEnvironment(environment);
    auto bytes = std::make_shared<QByteArray>();
    auto errors = std::make_shared<QByteArray>();
    auto* timer = new QTimer(child);
    timer->setSingleShot(true);
    auto complete = std::make_shared<bool>(false);
    auto finish = [this, child, timer, bytes, errors, temporary, complete, done, action](int code) {
      if (*complete)
        return;
      *complete = true;
      timer->stop();
      bytes->append(child->readAllStandardOutput());
      errors->append(child->readAllStandardError());
      process.clear();
      busy(false);
      QJsonParseError parse;
      const auto document = QJsonDocument::fromJson(*bytes, &parse);
      if (code != 0 || parse.error != QJsonParseError::NoError || !document.isObject()) {
        auto message = readableError(QString::fromUtf8(*errors));
        if (message.isEmpty())
          message = child->errorString();
        this->message(message.left(8192));
        polling = false;
        if (action == "build" || action == "start-test" || action == "test-status" || action == "cancel-test")
          tests_known = false;
      } else {
        remember(document.object(), action);
        done(document.object());
      }
      updateButtons();
      child->deleteLater();
    };
    QObject::connect(child, &QProcess::readyReadStandardOutput, owner, [child, bytes] {
      bytes->append(child->readAllStandardOutput());
      if (bytes->size() > 8 * 1024 * 1024)
        child->kill();
    });
    QObject::connect(child, &QProcess::readyReadStandardError, owner, [child, errors] {
      errors->append(child->readAllStandardError());
      if (errors->size() > 1024 * 1024)
        child->kill();
    });
    QObject::connect(
        child,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        owner,
        [finish](int code, QProcess::ExitStatus) { finish(code); });
    QObject::connect(child, &QProcess::errorOccurred, owner, [finish](QProcess::ProcessError error) {
      if (error == QProcess::FailedToStart)
        finish(-1);
    });
    QObject::connect(timer, &QTimer::timeout, child, &QProcess::kill);
    busy(true);
    child->start(launcher, {"setup", "--request", temporary->fileName()});
    timer->start((timeout + 1) * 1000);
  }
  void report(const QJsonObject& value) {
    QStringList lines;
    for (const auto& item : value.value("checks").toArray()) {
      const auto check = item.toObject();
      lines << (check.value("ok").toBool() ? "✓ " : "Needs attention: ") + check.value("name").toString() + " — " +
              check.value("message").toString();
      if (!check.value("ok").toBool() && !check.value("next").toString().isEmpty())
        lines << check.value("next").toString();
    }
    for (const auto& item : value.value("machines").toArray()) {
      const auto node = item.toObject();
      lines << node.value("name").toString() + " · " + node.value("gpu").toString() + " · " +
              node.value("partition").toString() + " · " + node.value("state").toString();
    }
    lines << value.value("next").toString();
    if (value.contains("message"))
      lines << value.value("message").toString();
    message(lines.join('\n'));
  }
  static constexpr const char* fields[] = {
      "label",
      "nodes",
      "partition",
      "gres",
      "cpus",
      "memory_mb",
      "time_seconds",
      "concurrency",
      "runtime_root",
      "per_user",
      "account",
      "qos",
      "constraint"};
  void load(const QJsonObject& value) {
    loading = true;
    server->setText(value.value("server").toString());
    registry->setText(value.value("registry").toString());
    QStringList roots;
    for (const auto& root : value.value("game_roots").toArray())
      roots << root.toString();
    recordings->setPlainText(roots.join('\n'));
    controller_runtime->setText(value.value("controller_runtime").toString());
    dashboard->setText(value.value("observer_registry").toString());
    observer = value.value("observer_registry");
    retained_profiles = value.value("profiles").toObject();
    profiles->setRowCount(0);
    for (auto entry = retained_profiles.begin(); entry != retained_profiles.end(); ++entry)
      row(entry.key(), entry.value().toObject());
    loading = false;
    invalidate();
  }
  void row(const QString& id, const QJsonObject& value) {
    const QSignalBlocker blocked(profiles);
    const int index = profiles->rowCount();
    profiles->insertRow(index);
    auto* name = new QTableWidgetItem(id);
    name->setFlags(name->flags() & ~Qt::ItemIsEditable);
    profiles->setItem(index, 0, name);
    for (int col = 0; col < 13; ++col) {
      auto data = value.value(fields[col]);
      QString text;
      if (col == 1) {
        QStringList nodes;
        for (const auto& node : data.toArray())
          nodes << node.toString();
        text = nodes.join(", ");
      } else
        text = data.isDouble() ? QString::number(data.toInt()) : data.toString();
      profiles->setItem(index, col + 1, new QTableWidgetItem(text));
    }
  }
  QJsonObject draft() const {
    QJsonObject items;
    for (int row = 0; row < profiles->rowCount(); ++row) {
      const auto id = profiles->item(row, 0)->text();
      auto value = retained_profiles.value(id).toObject();
      for (int col = 0; col < 13; ++col) {
        auto input = profiles->item(row, col + 1)->text().trimmed();
        if (col == 1) {
          QJsonArray nodes;
          for (const auto& node : input.split(',', Qt::SkipEmptyParts))
            nodes.append(node.trimmed());
          value.insert(fields[col], nodes);
        } else if ((col >= 4 && col <= 7) || col == 9) {
          bool valid;
          const int number = input.toInt(&valid);
          value.insert(fields[col], valid ? QJsonValue(number) : QJsonValue(input));
        } else
          value.insert(fields[col], input);
      }
      items.insert(id, value);
    }
    QJsonArray roots;
    for (const auto& path : recordings->toPlainText().split('\n', Qt::SkipEmptyParts))
      roots.append(path.trimmed());
    return {
        {"schema", 1},
        {"server", server->text().trimmed()},
        {"registry", registry->text().trimmed()},
        {"game_roots", roots},
        {"profiles", items},
        {"controller_runtime", controller_runtime->text().trimmed()},
        {"observer_registry",
         dashboard->text().trimmed().isEmpty() ? QJsonValue(QJsonValue::Null)
                                               : QJsonValue(dashboard->text().trimmed())}};
  }
  void operation(const QString& action, QJsonObject extra = {}, std::function<void(QJsonObject)> done = {}) {
    extra.insert("action", action);
    extra.insert("registry", registry->text().trimmed());
    extra.insert("timeout_seconds", timeout);
    extra.insert("draft", draft());
    call(extra, done ? done : [this](QJsonObject value) { report(value); });
  }
  void testStatus(const QJsonObject& value) {
    tests = value.value("tests").toArray();
    QStringList lines;
    bool running = false;
    for (const auto& item : tests) {
      const auto row = item.toObject();
      lines << row.value("label").toString(row.value("profile").toString()) + " / " + row.value("node").toString() +
              ": " + row.value("state").toString();
      const auto status = row.value("status").toObject();
      for (const auto& stage : status.value("nodes").toArray()) {
        const auto node = stage.toObject();
        lines << "  " + node.value("label").toString() + ": " + node.value("state").toString();
        if (node.contains("reason"))
          lines << node.value("reason").toString();
      }
      if (status.contains("controller_error"))
        lines << status.value("controller_error").toObject().value("error").toString();
      if (row.contains("log"))
        lines << row.value("log").toString();
      running |= !status.isEmpty() && !row.value("finished").toBool();
    }
    lines << value.value("message").toString();
    if (value.value("tested").toBool())
      lines << "All selected computers passed. Apply tested settings to enable these profiles.";
    message(lines.join('\n'), "tests");
    polling = running;
  }
};

WorkflowSetupDialog::WorkflowSetupDialog(
    const QString& launcher,
    const QString& registry,
    const QString& game,
    const QProcessEnvironment& environment,
    const YAML::Node& settings,
    QWidget* parent)
    : QDialog(parent, Qt::Window), impl_(std::make_unique<Impl>()) {
  auto& s = *impl_;
  s.owner = this;
  s.launcher = launcher;
  s.environment = environment;
  s.settings = YAML::Clone(settings);
  s.timeout = settings["workflow"]["setup"]["operation_timeout_seconds"].as<int>();
  setObjectName("workflowSetupDialog");
  setWindowTitle("Set up my HStream cluster");
  setSizeGripEnabled(true);
  resize(1050, 730);
  auto* outer = new QVBoxLayout(this);
  auto* scroll = new QScrollArea(this);
  scroll->setWidgetResizable(true);
  auto* contents = new QWidget(scroll);
  auto* layout = new QVBoxLayout(contents);
  scroll->setWidget(contents);
  outer->addWidget(scroll);
  auto* introduction = new QLabel(
      "Use the computers in your existing Slurm cluster to run stitching experiments. "
      "Start with your main server, choose which computers to use, then test the setup. "
      "Your registry is just the shared folder that remembers these choices and their test results.",
      this);
  introduction->setWordWrap(true);
  layout->addWidget(introduction);
  s.next_step = new QLabel(this);
  s.next_step->setObjectName("workflowSetupNext");
  s.next_step->setWordWrap(true);
  layout->addWidget(s.next_step);
  auto* connection = new QGroupBox("1. Find my cluster", this);
  auto* form = new QFormLayout(connection);
  s.server = new QLineEdit(connection);
  s.server->setObjectName("workflowSetupServer");
  s.server->setPlaceholderText("Main server name, for example ripper");
  const auto setup = settings["workflow"]["setup"];
  if (setup["server"] && !setup["server"].IsNull())
    s.server->setText(QString::fromStdString(setup["server"].as<std::string>()));
  QString suggested = QString::fromStdString(setup["registry_path"].as<std::string>());
  if (suggested.startsWith("~/"))
    suggested = QDir::home().filePath(suggested.mid(2));
  s.registry = new QLineEdit(registry.isEmpty() ? suggested : registry, connection);
  s.registry->setObjectName("workflowSetupFolder");
  s.registry->setToolTip("Keep this folder on shared storage. Reopening it recovers saved settings and setup drafts.");
  s.recordings = new QPlainTextEdit(QFileInfo(game).absolutePath(), connection);
  s.recordings->setMaximumHeight(65);
  s.recordings->setObjectName("workflowSetupRecordings");
  form->addRow("Main server", s.server);
  auto* locations = new QWidget(connection);
  auto* location_form = new QFormLayout(locations);
  location_form->addRow("Keep cluster settings in", s.registry);
  location_form->addRow("Recording folders (one per line)", s.recordings);
  s.controller_runtime = new QLineEdit(QString::fromStdString(setup["runtime_path"].as<std::string>()), connection);
  s.controller_runtime->setObjectName("workflowSetupRuntime");
  location_form->addRow("HStream installation on main server", s.controller_runtime);
  s.dashboard = new QLineEdit(connection);
  s.dashboard->setPlaceholderText("Optional: existing Dagster observation folder");
  location_form->addRow("Dagster dashboard", s.dashboard);
  auto* actions = new QHBoxLayout();
  s.discover = new QPushButton(action_icon(ActionIcon::Network), "Find computers / test connection", this);
  s.discover->setObjectName("workflowSetupDiscover");
  s.reload = new QPushButton(action_icon(ActionIcon::Open), "Recover saved setup", this);
  s.reload->setObjectName("workflowSetupRecover");
  actions->addWidget(s.discover);
  actions->addWidget(s.reload);
  form->addRow(actions);
  s.use_installed =
      new QPushButton(action_icon(ActionIcon::Apply), "Use installed HStream for main server", connection);
  s.use_installed->setObjectName("workflowSetupUseInstalled");
  s.installedRuntime();
  form->addRow(s.use_installed);
  auto* show_locations =
      new QPushButton(action_icon(ActionIcon::Open), "Storage, software and dashboard settings…", connection);
  show_locations->setCheckable(true);
  connect(show_locations, &QPushButton::toggled, locations, &QWidget::setVisible);
  form->addRow(show_locations);
  form->addRow(locations);
  locations->hide();
  layout->addWidget(connection);
  auto* group = new QGroupBox("2. Choose how to run experiments", this);
  auto* choices = new QVBoxLayout(group);
  auto* explanation = new QLabel(
      "A profile is a named choice of computers and job limits—for example ‘Fast GPUs’. "
      "Add a profile or edit the saved rows below. Saving a draft does not change running jobs.",
      group);
  explanation->setWordWrap(true);
  choices->addWidget(explanation);
  s.computers = new QComboBox(group);
  s.computers->setObjectName("workflowSetupComputers");
  s.computers->addItem("Find computers first, or add a profile manually");
  choices->addWidget(s.computers);
  s.profiles = new QTableWidget(0, 14, group);
  s.profiles->setMaximumHeight(180);
  s.profiles->setObjectName("workflowSetupProfiles");
  s.profiles->setHorizontalHeaderLabels(
      {"ID",
       "Name",
       "Computers (comma separated)",
       "Queue",
       "GPU request",
       "CPU cores",
       "Memory (MB)",
       "Time limit (seconds)",
       "Parallel jobs",
       "HStream installation",
       "Jobs per user",
       "Slurm account",
       "Quality of service",
       "Hardware constraint"});
  for (int col : {0, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13})
    s.profiles->setColumnHidden(col, true);
  s.profiles->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  choices->addWidget(s.profiles);
  auto* editing = new QHBoxLayout();
  s.add = new QPushButton(action_icon(ActionIcon::Add), "Add selected computer", group);
  s.remove = new QPushButton(action_icon(ActionIcon::Remove), "Remove selected profile", group);
  s.save = new QPushButton(action_icon(ActionIcon::Save), "Save setup draft", group);
  s.save->setObjectName("workflowSetupSave");
  for (auto* button : {s.add, s.remove, s.save})
    editing->addWidget(button);
  auto* advanced = new QPushButton(action_icon(ActionIcon::Inspect), "Advanced profile settings", group);
  advanced->setCheckable(true);
  editing->addWidget(advanced);
  connect(advanced, &QPushButton::toggled, this, [&s](bool show) {
    for (int col : {0, 3, 4, 5, 6, 7, 9, 10, 11, 12, 13})
      s.profiles->setColumnHidden(col, !show);
  });
  choices->addLayout(editing);
  layout->addWidget(group, 1);
  auto* testing = new QGroupBox("3. Build and test", this);
  auto* testing_layout = new QFormLayout(testing);
  s.sample = new QLineEdit(game, testing);
  testing_layout->addRow("Sample recording", s.sample);
  auto* testing_help = new QLabel(
      "Choose a recording with players on the ice. The test makes private copies, runs a short calibration on each computer and checks cleanup. No result is promoted to your recording.",
      testing);
  testing_help->setWordWrap(true);
  testing_layout->addRow(testing_help);
  auto* test_buttons = new QHBoxLayout;
  s.build = new QPushButton(action_icon(ActionIcon::Add), "Save and build registry", testing);
  s.build->setObjectName("workflowSetupBuild");
  s.test = new QPushButton(action_icon(ActionIcon::Play), "Test next computer", testing);
  s.test->setObjectName("workflowSetupTest");
  s.resume = new QPushButton(action_icon(ActionIcon::Inspect), "Resume / check tests", testing);
  s.resume->setObjectName("workflowSetupResume");
  s.stop = new QPushButton(action_icon(ActionIcon::Stop), "Stop test", testing);
  s.stop->setObjectName("workflowSetupStopTest");
  for (auto* button : {s.build, s.test, s.resume, s.stop})
    test_buttons->addWidget(button);
  testing_layout->addRow(test_buttons);
  auto* install_buttons = new QHBoxLayout;
  s.apply = new QPushButton(action_icon(ActionIcon::Apply), "Apply tested settings", testing);
  s.apply->setObjectName("workflowSetupApply");
  s.start = new QPushButton(action_icon(ActionIcon::Play), "Start controller", testing);
  s.start->setObjectName("workflowSetupStart");
  s.stop_service = new QPushButton(action_icon(ActionIcon::Stop), "Stop controller", testing);
  install_buttons->addWidget(s.stop_service);
  install_buttons->addWidget(s.apply);
  install_buttons->addWidget(s.start);
  testing_layout->addRow(install_buttons);
  layout->addWidget(testing);
  s.results = new LogTextEdit(this);
  s.results->setObjectName("workflowSetupResults");
  s.results->setReadOnly(true);
  s.results->setMaximumHeight(200);
  s.results->setPlainText(
      "Enter your main server, then click Find computers / test connection. "
      "If you already set up this cluster, click Recover saved setup.");
  layout->addWidget(s.results);
  auto* buttons = new QHBoxLayout();
  s.check = new QPushButton(action_icon(ActionIcon::Inspect), "Check installed setup", this);
  s.check->setObjectName("workflowSetupCheck");
  s.use = new QPushButton(action_icon(ActionIcon::Apply), "Use this cluster", this);
  s.use->setObjectName("workflowSetupUse");
  s.use->setEnabled(false);
  auto* close = new QPushButton(action_icon(ActionIcon::Close), "Close", this);
  buttons->addWidget(s.check);
  buttons->addWidget(s.use);
  buttons->addStretch();
  buttons->addWidget(close);
  outer->addLayout(buttons);
  connect(close, &QPushButton::clicked, this, &QDialog::reject);
  connect(s.use, &QPushButton::clicked, this, &QDialog::accept);
  for (auto* control : {s.server, s.registry, s.controller_runtime, s.dashboard})
    connect(control, &QLineEdit::textChanged, this, [&s] { s.invalidate(); });
  connect(s.server, &QLineEdit::textChanged, this, [&s] { s.installedRuntime(); });
  connect(s.use_installed, &QPushButton::clicked, this, [&s] {
    if (s.installed_runtime.isEmpty() || s.installed_server != s.server->text().trimmed())
      return;
    s.controller_runtime->setText(s.installed_runtime);
    s.invalidate();
    s.message(
        "Main-server installation set to " + s.installed_runtime +
        ". Save and build registry to check it. Setup automatically prepares the installed programs for each selected computer. "
        "Each computer still needs to pass its test.");
  });
  connect(s.registry, &QLineEdit::textChanged, this, [&s] {
    if (!s.loading) {
      s.revision = QJsonValue::Null;
      s.saved_draft = {};
      s.progress = {};
      s.tests_known = false;
      s.polling = false;
      s.controller_state = 0;
      s.updateButtons();
    }
  });
  connect(s.recordings, &QPlainTextEdit::textChanged, this, [&s] { s.invalidate(); });
  connect(s.profiles, &QTableWidget::itemChanged, this, [&s] { s.invalidate(); });
  connect(s.profiles, &QTableWidget::itemSelectionChanged, this, [&s] { s.updateButtons(); });
  connect(s.sample, &QLineEdit::textChanged, this, [&s] { s.updateButtons(); });
  connect(s.discover, &QPushButton::clicked, this, [&s] {
    s.installedRuntime();
    s.results->setPlainText("Connecting to " + s.server->text() + " and asking which computers are available…");
    s.call(
        {{"action", "discover"}, {"server", s.server->text().trimmed()}, {"timeout_seconds", s.timeout}},
        [&s](QJsonObject value) {
          s.report(value);
          s.installedRuntime(value.value("installed_runtime").toString());
          s.computers->clear();
          for (const auto& item : value.value("machines").toArray()) {
            const auto machine = item.toObject();
            s.computers->addItem(
                machine.value("name").toString() + " · " + machine.value("gpu").toString() + " · " +
                    machine.value("partition").toString(),
                machine);
          }
          const auto discovered = value.value("service_registry").toString();
          if (!discovered.isEmpty()) {
            s.registry->setText(discovered);
            s.results->appendLogText(
                "Found existing cluster settings at " + discovered + ". Click Recover saved setup.");
          }
        });
  });
  connect(s.reload, &QPushButton::clicked, this, [&s] {
    const QString folder = s.registry->text().trimmed();
    const bool draft = QFileInfo::exists(QDir(folder).filePath("setup.json"));
    s.call(
        {{"action", draft ? "load" : "import"}, {"registry", folder}, {"server", s.server->text().trimmed()}},
        [&s](QJsonObject value) {
          s.revision = value.contains("revision") ? value.value("revision") : QJsonValue(QJsonValue::Null);
          s.load(value.value("draft").toObject());
          QStringList messages{"Recovered saved setup. You can edit it and save a new draft."};
          for (const auto& issue : value.value("issues").toArray())
            messages << issue.toObject().value("profile").toString() + ": " +
                    issue.toObject().value("message").toString();
          s.results->setPlainText(messages.join('\n'));
        });
  });
  connect(s.save, &QPushButton::clicked, this, [&s] {
    s.call(
        {{"action", "save"},
         {"registry", s.registry->text().trimmed()},
         {"draft", s.draft()},
         {"revision", s.revision}},
        [&s](QJsonObject value) {
          s.revision = value.value("revision");
          s.results->setPlainText(
              "Setup draft saved. Recover saved setup will restore these choices next time. "
              "This draft has not been installed or tested on the GPUs yet.");
        });
  });
  connect(s.add, &QPushButton::clicked, this, [&s, setup] {
    QString id;
    int count = s.profiles->rowCount();
    do {
      id = "gpu-" + QString::number(++count);
    } while (s.retained_profiles.contains(id));
    const auto defaults = setup["gpu_resources"];
    const auto machine = s.computers->currentData().toJsonObject();
    auto gpu = machine.value("gpu").toString().split(',').first().section('(', 0, 0);
    const int last = gpu.lastIndexOf(':');
    if (last >= 0)
      gpu = gpu.left(last) + ":1";
    QJsonObject value{
        {"label", machine.isEmpty() ? "New GPU profile" : machine.value("name").toString()},
        {"nodes", machine.isEmpty() ? QJsonArray{} : QJsonArray{machine.value("name")}},
        {"partition", machine.value("partition").toString().remove('*')},
        {"gres", machine.isEmpty() || gpu == "(null)" ? "" : gpu},
        {"cpus", defaults["cpus"].as<int>()},
        {"memory_mb", defaults["memory_mb"].as<int>()},
        {"time_seconds", defaults["time_seconds"].as<int>()},
        {"concurrency", s.settings["workflow"]["max_parallel"].as<int>()},
        {"per_user", s.settings["workflow"]["max_parallel"].as<int>()},
        {"account", ""},
        {"qos", ""},
        {"constraint", ""},
        {"runtime_root", QString::fromStdString(setup["runtime_path"].as<std::string>())}};
    s.retained_profiles.insert(id, value);
    s.row(id, value);
    s.invalidate();
  });
  connect(s.remove, &QPushButton::clicked, this, [&s] {
    if (s.profiles->currentRow() >= 0)
      s.profiles->removeRow(s.profiles->currentRow());
    s.invalidate();
  });
  connect(s.check, &QPushButton::clicked, this, [&s] {
    const QString folder = s.registry->text().trimmed();
    s.call(
        {{"action", "inspect"},
         {"registry", folder},
         {"draft", s.draft()},
         {"timeout_seconds", s.settings["workflow"]["command_timeout_seconds"].as<int>()}},
        [&s, folder](QJsonObject value) {
          s.report(value);
          if (value.value("ready").toBool()) {
            s.accepted_registry = folder;
            s.use->setEnabled(true);
          }
        });
  });
  connect(s.build, &QPushButton::clicked, this, [&s] {
    QJsonObject defaults, resources;
    for (const char* key :
         {"max_parallel",
          "poll_interval_ms",
          "command_timeout_seconds",
          "shutdown_timeout_seconds",
          "observer_timeout_seconds"})
      defaults.insert(key, s.settings["workflow"][key].as<int>());
    for (const char* key : {"cpus", "memory_mb", "time_seconds"})
      resources.insert(key, s.settings["workflow"]["metadata_resources"][key].as<int>());
    s.operation("save", {{"revision", s.revision}}, [&s, defaults, resources](QJsonObject saved) {
      s.revision = saved.value("revision");
      s.message("Preparing installed HStream programs on the selected computers and building the registry…");
      s.operation(
          "build",
          {{"revision", s.revision}, {"defaults", defaults}, {"metadata_resources", resources}},
          [&s](QJsonObject value) {
            if (value.value("draft").isObject())
              s.load(value.value("draft").toObject());
            s.tests = {};
            s.report(value);
          });
    });
  });
  connect(s.resume, &QPushButton::clicked, this, [&s] {
    s.operation("test-status", {}, [&s](QJsonObject value) { s.testStatus(value); });
  });
  connect(s.stop, &QPushButton::clicked, this, [&s] {
    s.operation("cancel-test", {}, [&s](QJsonObject value) { s.testStatus(value); });
  });
  connect(s.test, &QPushButton::clicked, this, [&s] {
    s.operation("test-status", {}, [&s](QJsonObject value) {
      s.testStatus(value);
      if (s.polling)
        return;
      for (const auto& item : s.tests) {
        const auto row = item.toObject();
        if (row.value("passed").toBool())
          continue;
        const auto snapshot = hm::user_config::load_snapshot();
        if (!snapshot.ok()) {
          s.message(QString::fromStdString(snapshot.status().ToString()));
          return;
        }
        s.operation(
            "start-test",
            {{"profile", row.value("profile")},
             {"node", row.value("node")},
             {"game", s.sample->text().trimmed()},
             {"scan_seconds", s.settings["hstream_ui"]["stitching_experiments"]["scan_duration_seconds"].as<int>()},
             {"user_config_snapshot",
              QJsonObject{
                  {"schema", 1},
                  {"source_path", QString::fromStdString(snapshot->path.string())},
                  {"values", jsonValue(snapshot->values)}}}},
            [&s](QJsonObject next) { s.testStatus(next); });
        return;
      }
    });
  });
  connect(s.apply, &QPushButton::clicked, this, [&s] { s.operation("activate"); });
  connect(s.start, &QPushButton::clicked, this, [&s] { s.operation("start-service"); });
  connect(s.stop_service, &QPushButton::clicked, this, [&s] { s.operation("stop-service"); });
  s.poll = new QTimer(this);
  s.poll->setInterval(settings["workflow"]["poll_interval_ms"].as<int>());
  connect(s.poll, &QTimer::timeout, this, [&s] {
    if (s.polling && !s.process)
      s.operation("test-status", {}, [&s](QJsonObject value) { s.testStatus(value); });
  });
  s.poll->start();
  s.updateButtons();
  if (QFileInfo::exists(QDir(s.registry->text()).filePath("setup.json")) ||
      QFileInfo::exists(QDir(s.registry->text()).filePath("config.json")))
    QTimer::singleShot(0, s.reload, &QPushButton::click);
}

WorkflowSetupDialog::~WorkflowSetupDialog() {
  if (impl_->process) {
    impl_->process->disconnect(this);
    impl_->process->kill();
    impl_->process->waitForFinished(1000);
  }
}

QString WorkflowSetupDialog::selectedRegistry() const {
  return impl_->accepted_registry;
}
