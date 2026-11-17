#include "src/apps/hstream-ui/WorkflowSetupDialog.h"

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QPointer>
#include <QtCore/QProcess>
#include <QtCore/QSet>
#include <QtCore/QSignalBlocker>
#include <QtCore/QTemporaryFile>
#include <QtCore/QTimer>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QFileDialog>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QGroupBox>
#include <QtWidgets/QHeaderView>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QStackedWidget>
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
  LogTextEdit *results, *prepare_log;
  QString test_activity, next_check;
  QLabel* next_step;
  QPushButton *discover, *reload, *save, *check, *use, *add, *remove, *use_installed;
  QPushButton *build, *test, *resume, *stop, *apply, *start, *stop_service;
  QJsonValue revision{QJsonValue::Null};
  QJsonObject retained_profiles;
  QJsonObject unchecked_profiles;
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

  enum class Flow { None, Connect, SaveChoices, Prepare, Tests, Enable };
  Flow flow{Flow::None};
  QStackedWidget* pages;
  QLabel *heading, *step_label, *summary, *recording_name, *test_summary, *finish_summary, *problem;
  QPushButton *next, *back, *wizard_stop;
  QListWidget* machine_list;
  QSet<QString> attempted;
  bool recovering_tests{false};
  bool cancel_pending{false};

  bool currentBuild() const {
    return saved_draft == draft() && progress.value("revision") == revision && progress.value("build_current").toBool();
  }
  void showStep(int index) {
    pages->setCurrentIndex(index);
    problem->clear();
    updateButtons();
  }
  void showProblem(const QString& text) {
    problem->setText(text);
    problem->setVisible(!text.isEmpty());
  }
  void updateWizard() {
    const int step = pages->currentIndex();
    const QStringList titles{
        "Which computer is in charge?",
        "Which computers should help?",
        "Let's get them ready",
        "Check your computers",
        "Use your computers"};
    heading->setText(titles[step]);
    step_label->setText(QString("Step %1 of 5 · Connect → Computers → Prepare → Test → Finish").arg(step + 1));
    const bool idle = !process && flow == Flow::None;
    const bool applied = currentBuild() && progress.value("applied").toBool();
    const auto items = draft().value("profiles").toObject();
    QStringList selected;
    for (const auto& item : items)
      for (const auto& node : item.toObject().value("nodes").toArray())
        if (!selected.contains(node.toString()))
          selected << node.toString();
    if (tests.isEmpty()) {
      QStringList waiting;
      for (const auto& name : selected)
        waiting << name + ": Waiting to test";
      test_summary->setText(waiting.join('\n'));
    }
    recording_name->setText(
        sample->text().isEmpty() ? "Choose one game recording below."
                                 : "Recording: " + QFileInfo(sample->text()).fileName());
    summary->setText(
        "Main server: " + server->text() + "\nComputers: " + selected.join(", ") +
        "\n\nHStream will find the installed programs and prepare a shared place to keep your settings. "
        "This can take a few minutes.");
    QString instruction, label;
    bool enabled = false;
    if (step == 0) {
      label = "Find my computers";
      enabled = discover->isEnabled();
      instruction = "Enter the name of your main server, then click Find my computers.";
    } else if (step == 1) {
      label = "Continue";
      enabled = !selected.isEmpty();
      instruction = selected.isEmpty() ? "Tick at least one computer to use."
                                       : "Selected: " + selected.join(", ") + ". Click Continue.";
    } else if (step == 2) {
      const bool unknown = progress.value("has_build").toBool() && !tests_known;
      label = unknown ? "Check saved progress" : currentBuild() ? "Continue" : "Prepare computers";
      enabled = unknown ? resume->isEnabled() : currentBuild() || build->isEnabled();
      instruction = unknown ? "We found an earlier setup. Check where it stopped before continuing."
          : recordings->toPlainText().trimmed().isEmpty() ? "Choose the folder containing your recordings."
                                                          : "Click " + label + ".";
    } else if (step == 3) {
      label = !currentBuild() ? "Prepare changed choices"
          : !tests_known      ? "Continue the test"
          : tests_passed      ? "Continue"
                              : "Test my computers";
      enabled = !currentBuild() || tests_passed || (!tests_known ? resume->isEnabled() : test->isEnabled());
      instruction = test_running ? "Testing is in progress. You can leave this open or stop the test."
          : tests_passed         ? "Every computer passed. Click Continue."
          : !problem->text().isEmpty()
          ? "Read the test problem above before trying again. Passed checks are kept."
          : "Run the quick HStream and GPU checks. No video processing; passed checks are kept.";
    } else {
      label = !accepted_registry.isEmpty() ? "Use these computers"
          : applied                        ? "Finish setup"
                                           : "Enable these computers";
      enabled =
          !accepted_registry.isEmpty() || applied || (currentBuild() && tests_known && tests_passed && !test_running);
      instruction = !accepted_registry.isEmpty() ? "Ready. Click Use these computers to return to your experiments."
                                                 : "Click " + label +
              ". HStream will save the tested settings, start the background service and check that it works.";
      finish_summary->setText(
          !accepted_registry.isEmpty()
              ? "Your computers are ready to run stitching experiments."
              : "Every selected computer has passed. Enabling checks for running experiments before changing settings. "
                "It restarts HStream’s background service if needed. This does not start an experiment.");
    }
    if (!idle || test_running) {
      if (flow == Flow::Tests || test_running)
        instruction = test_activity.isEmpty() ? "Checking saved test results… Passed checks are kept." : test_activity;
      else if (flow == Flow::Enable)
        instruction = "Turning on cluster execution…";
      else if (flow == Flow::Prepare)
        instruction = "Preparing your computers… this can take a few minutes.";
      else
        instruction = "Checking… please wait.";
    }
    next_step->setText(instruction);
    next->setText(label);
    next->setEnabled(idle && enabled && (!test_running || (step == 3 && !tests_known)));
    next->setToolTip(enabled ? QString{} : instruction);
    next->setIcon(action_icon(step == 4 ? ActionIcon::Apply : step == 3 ? ActionIcon::Play : ActionIcon::Next));
    back->setEnabled(idle && step > 0 && !test_running);
    wizard_stop->setVisible(step == 3 && (test_running || flow == Flow::Tests || cancel_pending));
    wizard_stop->setEnabled(!cancel_pending);
    problem->setVisible(!problem->text().isEmpty());
    machine_list->setEnabled(idle && !test_running);
    for (const char* name :
         {"workflowSetupBrowseRecordings",
          "workflowSetupBrowseSample",
          "workflowSetupContinueSaved",
          "workflowSetupSearchAgain"})
      if (auto* button = owner->findChild<QPushButton*>(name))
        button->setEnabled(idle && !test_running);
    for (auto* control : {server, registry, controller_runtime, dashboard, sample})
      control->setEnabled(idle && !test_running);
    computers->setEnabled(idle && !test_running);
    profiles->setEnabled(idle && !test_running);
    recordings->setEnabled(idle && !test_running);
  }
  void syncMachines() {
    const QSignalBlocker blocked(machine_list);
    const auto selected = draft().value("profiles").toObject();
    QSet<QString> names, chosen;
    for (const auto& item : selected)
      for (const auto& node : item.toObject().value("nodes").toArray())
        chosen.insert(node.toString());
    machine_list->clear();
    auto append = [&](const QString& name, const QString& description, int index) {
      if (names.contains(name))
        return;
      names.insert(name);
      auto* item = new QListWidgetItem(name + (description.isEmpty() ? "" : " — " + description), machine_list);
      item->setData(Qt::UserRole, name);
      item->setData(Qt::UserRole + 1, index);
      item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
      item->setCheckState(chosen.contains(name) ? Qt::Checked : Qt::Unchecked);
    };
    for (int i = 0; i < computers->count(); ++i) {
      const auto machine = computers->itemData(i).toJsonObject();
      if (!machine.isEmpty()) {
        const auto state = machine.value("state").toString();
        const QString status = state.startsWith("idle", Qt::CaseInsensitive) ? "Available"
            : (state.startsWith("alloc", Qt::CaseInsensitive) || state.startsWith("mix", Qt::CaseInsensitive))
            ? "Busy — tests may wait"
            : (state.startsWith("down", Qt::CaseInsensitive) || state.startsWith("drain", Qt::CaseInsensitive) ||
               state.startsWith("fail", Qt::CaseInsensitive))
            ? "Unavailable"
            : state;
        append(machine.value("name").toString(), status, i);
      }
    }
    for (const auto& name : chosen)
      append(name, "saved choice", -1);
    for (auto entry = unchecked_profiles.begin(); entry != unchecked_profiles.end(); ++entry)
      append(entry.key(), "saved choice", -1);
  }
  void nextStep() {
    showProblem({});
    const int step = pages->currentIndex();
    if (step == 0) {
      flow = Flow::Connect;
      discover->click();
    } else if (step == 1) {
      flow = Flow::SaveChoices;
      save->click();
    } else if (step == 2) {
      if (progress.value("has_build").toBool() && !tests_known) {
        flow = Flow::Prepare;
        resume->click();
      } else if (currentBuild())
        showStep(3);
      else {
        flow = Flow::Prepare;
        build->click();
      }
    } else if (step == 3) {
      if (!currentBuild())
        showStep(2);
      else if (tests_passed)
        showStep(4);
      else {
        attempted.clear();
        recovering_tests = !tests_known;
        flow = Flow::Tests;
        if (!tests_known)
          resume->click();
        else
          test->click();
      }
    } else if (!accepted_registry.isEmpty())
      owner->accept();
    else {
      flow = Flow::Enable;
      enableCluster();
    }
    updateButtons();
  }
  void enableCluster() {
    if (!currentBuild()) {
      flow = Flow::None;
      showStep(2);
      return;
    }
    if (!progress.value("applied").toBool()) {
      if (progress.value("installed").toBool() && controller_state != -1)
        stop_service->click();
      else
        apply->click();
    } else if (controller_state != 1)
      start->click();
    else
      check->click();
  }
  void after(const QString& action, const QJsonObject& value) {
    if (process)
      return; // A save or test-status callback already started its next command.
    if (cancel_pending) {
      cancel_pending = false;
      operation("cancel-test", {}, [this](QJsonObject result) { testStatus(result); });
      return;
    }
    if (action == "load" || action == "import") {
      syncMachines();
      showStep(
          progress.value("applied").toBool()         ? 4
              : progress.value("has_build").toBool() ? 3
              : profiles->rowCount()                 ? 2
                                                     : 1);
    }
    if (flow == Flow::SaveChoices && action == "save") {
      flow = Flow::None;
      showStep(2);
    } else if (flow == Flow::Connect && action == "discover") {
      flow = Flow::None;
      QStringList failures;
      for (const auto& check : value.value("checks").toArray()) {
        const auto item = check.toObject();
        if (!item.value("ok").toBool())
          failures << item.value("name").toString() + ": " +
                  item.value("next").toString(item.value("message").toString());
      }
      if (!failures.isEmpty())
        showProblem(failures.join('\n'));
      else if (value.value("machines").toArray().isEmpty())
        showProblem("No computers were found. Check that the main server can see your cluster, then try again.");
      else {
        syncMachines();
        showStep(1);
      }
    } else if (flow == Flow::Prepare && (action == "build" || action == "test-status")) {
      flow = Flow::None;
      showStep(currentBuild() || test_running ? 3 : 2);
    } else if (flow == Flow::Tests && value.contains("tests")) {
      if (recovering_tests) {
        recovering_tests = false;
        for (const auto& item : tests) {
          const auto row = item.toObject();
          if (row.value("finished").toBool() && !row.value("passed").toBool()) {
            flow = Flow::None;
            showProblem(
                row.value("node").toString() + " did not pass while setup was away. " +
                row.value("next").toString("Check Test details below, then click Test my computers to try again."));
            return;
          }
        }
      }
      if (!currentBuild()) {
        flow = Flow::None;
        showStep(2);
        return;
      }
      if (tests.isEmpty()) {
        flow = Flow::None;
        showProblem("No computer tests were found. Go back and prepare your computers again.");
        return;
      }
      if (!tests_passed && !test_running && !QDir::isAbsolutePath(sample->text().trimmed())) {
        flow = Flow::None;
        showProblem("Choose a shared recording folder for the storage check, then test again.");
        return;
      }
      if (tests_passed) {
        flow = Flow::None;
        showStep(4);
      } else if (test_running) {
        for (const auto& item : tests) {
          const auto row = item.toObject();
          if (row.contains("status") && !row.value("finished").toBool())
            attempted.insert(row.value("profile").toString() + "/" + row.value("node").toString());
        }
      } else {
        for (const auto& item : tests) {
          const auto row = item.toObject();
          if (!row.value("passed").toBool() &&
              attempted.contains(row.value("profile").toString() + "/" + row.value("node").toString())) {
            flow = Flow::None;
            showProblem(
                row.value("node").toString() + " did not pass. " +
                row.value("next").toString(
                    "Open Test details below to see what happened. You can fix the problem and test again; passed computers will be kept."));
            return;
          }
        }
        QTimer::singleShot(0, owner, [this] {
          if (flow == Flow::Tests && !process)
            testNext();
        });
      }
    } else if (flow == Flow::Enable) {
      if (action == "inspect") {
        flow = Flow::None;
        if (accepted_registry.isEmpty() && problem->text().isEmpty())
          showProblem(
              "The background service is not ready yet. Read the check below, then click Finish setup to check again.");
      } else
        QTimer::singleShot(0, owner, [this] {
          if (flow == Flow::Enable && !process)
            enableCluster();
        });
    }
  }

  void updateButtons() {
    if (loading)
      return;
    const bool idle = !process && !test_running;
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
    enable(discover, server_set && !test_running, "Enter your main server name first.");
    enable(reload, folder && !test_running, "Choose the shared settings folder first.");
    enable(
        save,
        folder && QDir::isAbsolutePath(controller_runtime->text().trimmed()),
        "Choose the settings and HStream installation folders first.");
    enable(add, !test_running && flow == Flow::None && profiles->rowCount() < 64, "At most 64 profiles can be saved.");
    enable(
        remove, !test_running && flow == Flow::None && profiles->currentRow() >= 0, "Select a profile row to remove.");
    enable(
        build,
        choices && (!built || (tests_known && !test_running)),
        built && (!tests_known || test_running)
            ? "Resume / check tests, and stop any unfinished test before rebuilding."
            : "Enter the server and recording folder, and add at least one computer profile.");
    enable(
        test,
        build_current && tests_known && !tests_passed && !test_running && flow != Flow::Tests &&
            QDir::isAbsolutePath(sample->text().trimmed()),
        !build_current     ? "Save and build the current choices first."
            : !tests_known ? "Resume / check tests first."
            : test_running ? "Wait for the unfinished test, or stop it."
            : tests_passed ? "Every selected computer has passed. Apply tested settings next."
                           : "Choose a shared recording folder for the storage check first.");
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
    resume->setEnabled(!process && built && saved_server && !polling && flow == Flow::None);
    resume->setToolTip(
        polling || flow == Flow::Tests ? "Test progress updates automatically." : "Check saved test progress.");
    stop->setToolTip(
        cancel_pending ? "Waiting for the current request, then stopping."
                       : "Stop the unfinished check; passed checks are kept.");
    stop->setEnabled(built && saved_server && (!tests_known || test_running || flow == Flow::Tests) && !cancel_pending);
    resume->setText(polling || flow == Flow::Tests ? "Checking automatically" : "Resume / check tests");
    test->setText("Test next pending check");
    test->setToolTip(next_check.isEmpty() ? test->toolTip() : "Next: " + next_check);
    use_installed->setEnabled(idle && !installed_runtime.isEmpty() && installed_server == server->text().trimmed());
    updateWizard();
  }
  void remember(const QJsonObject& value, const QString& action) {
    if (action == "load" || action == "import") {
      tests_known = false;
      tests_passed = false;
      test_running = false;
      tests = {};
      test_activity.clear();
      next_check.clear();
      controller_state = 0;
    }
    if (value.value("draft").isObject())
      saved_draft = value.value("draft").toObject();
    if (value.contains("revision"))
      revision = value.value("revision");
    if (value.value("progress").isObject())
      progress = value.value("progress").toObject();
    if (action == "build") {
      test_activity.clear();
      next_check.clear();
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
  void message(const QString& text, const QString& source = "setup") {
    if (results->toPlainText() != text)
      qInfo().noquote() << "Workflow setup" << source << text;
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
    qInfo().noquote() << "Workflow setup action=" << action << "registry=" << request.value("registry").toString();
    if (action == "start-test" && flow == Flow::Tests)
      attempted.insert(request.value("profile").toString() + "/" + request.value("node").toString());
    if (action == "inspect" || action == "start-service" || action == "stop-service")
      controller_state = 0;
    auto temporary = std::make_shared<QTemporaryFile>();
    if (!temporary->open()) {
      const auto error = "Could not create the setup request: " + temporary->errorString();
      flow = Flow::None;
      polling = false;
      message(error);
      showProblem(error);
      updateButtons();
      return;
    }
    temporary->write(QJsonDocument(request).toJson(QJsonDocument::Compact));
    temporary->close();
    auto* child = new QProcess(owner);
    process = child;
    child->setProcessEnvironment(environment);
    auto bytes = std::make_shared<QByteArray>();
    auto errors = std::make_shared<QByteArray>();
    auto pending_errors = std::make_shared<QByteArray>();
    auto error_size = std::make_shared<qint64>(0);
    auto read_errors = [this, child, errors, pending_errors, error_size] {
      const auto chunk = child->readAllStandardError();
      *error_size += chunk.size();
      if (*error_size > 1024 * 1024) {
        child->kill();
        return;
      }
      pending_errors->append(chunk);
      int end;
      while ((end = pending_errors->indexOf('\n')) >= 0) {
        const auto line = pending_errors->left(end);
        pending_errors->remove(0, end + 1);
        const QByteArray prefix("HSTREAM_SETUP_PROGRESS ");
        const auto record =
            line.startsWith(prefix) ? QJsonDocument::fromJson(line.mid(prefix.size())).object() : QJsonObject{};
        if (record.value("message").isString()) {
          const auto text = record.value("message").toString();
          prepare_log->appendLogText(text);
          qInfo().noquote() << "Workflow setup preparation" << text;
        } else {
          errors->append(line + '\n');
        }
      }
    };
    auto* timer = new QTimer(child);
    timer->setSingleShot(true);
    auto complete = std::make_shared<bool>(false);
    auto finish =
        [this, child, timer, bytes, errors, temporary, complete, done, action, read_errors, pending_errors](int code) {
          if (*complete)
            return;
          *complete = true;
          timer->stop();
          bytes->append(child->readAllStandardOutput());
          read_errors();
          errors->append(*pending_errors);
          process.clear();
          QJsonParseError parse;
          const auto document = QJsonDocument::fromJson(*bytes, &parse);
          if (code != 0 || parse.error != QJsonParseError::NoError || !document.isObject()) {
            auto message = readableError(QString::fromUtf8(*errors));
            if (message.isEmpty())
              message = child->errorString();
            this->message(message.left(8192));
            showProblem(message.left(1200));
            flow = Flow::None;
            polling = false;
            if (action == "build" || action == "start-test" || action == "test-status" || action == "cancel-test")
              tests_known = false;
          } else {
            remember(document.object(), action);
            done(document.object());
            after(action, document.object());
          }
          if (cancel_pending && !process)
            after(action, {});
          updateButtons();
          child->deleteLater();
        };
    QObject::connect(child, &QProcess::readyReadStandardOutput, owner, [child, bytes] {
      bytes->append(child->readAllStandardOutput());
      if (bytes->size() > 8 * 1024 * 1024)
        child->kill();
    });
    QObject::connect(child, &QProcess::readyReadStandardError, owner, read_errors);
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
    updateButtons();
    child->start(launcher, {"setup", "--request", temporary->fileName()});
    timer->start((timeout + 1) * 1000);
  }
  void report(const QJsonObject& value) {
    QStringList lines, failures;
    for (const auto& item : value.value("checks").toArray()) {
      const auto check = item.toObject();
      lines << (check.value("ok").toBool() ? "✓ " : "Needs attention: ") + check.value("name").toString() + " — " +
              check.value("message").toString();
      if (!check.value("ok").toBool()) {
        failures << check.value("name").toString() + ": " + check.value("message").toString() + "\n" +
                check.value("next").toString();
        lines << check.value("next").toString();
      }
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
    if (!failures.isEmpty())
      showProblem(failures.join('\n').left(1600));
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
    unchecked_profiles = {};
    profiles->setRowCount(0);
    for (auto entry = retained_profiles.begin(); entry != retained_profiles.end(); ++entry)
      row(entry.key(), entry.value().toObject());
    loading = false;
    syncMachines();
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
  void testNext() {
    auto& s = *this;
    s.operation("test-status", {}, [&s](QJsonObject value) {
      s.testStatus(value);
      if (s.polling || s.cancel_pending)
        return;
      for (const auto& item : s.tests) {
        const auto row = item.toObject();
        if (row.value("passed").toBool())
          continue;
        const auto snapshot = hm::user_config::load_snapshot();
        if (!snapshot.ok()) {
          s.flow = Impl::Flow::None;
          const auto error = QString::fromStdString(snapshot.status().ToString());
          s.message(error);
          s.showProblem(error);
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
  }
  void testStatus(const QJsonObject& value) {
    tests = value.value("tests").toArray();
    if (sample->text().trimmed().isEmpty())
      for (const auto& item : tests)
        if (item.toObject().value("game").isString()) {
          sample->setText(item.toObject().value("game").toString());
          break;
        }
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
    const auto details = value.value("test_progress").toObject();
    test_activity = details.value("current").toString();
    next_check = details.value("next").toString();
    QStringList status_lines;
    for (const auto& item : tests) {
      const auto row = item.toObject();
      status_lines << row.value("node").toString() + ": " +
              (row.value("passed").toBool()                        ? "Passed"
                   : row.value("state").toString() == "not-tested" ? "Waiting for its turn"
                   : row.value("finished").toBool()                ? "Needs attention"
                                                                   : "Testing / waiting for a computer");
    }
    test_summary->setText(
        details.value("summary").toString(status_lines.join('\n')) +
        (test_activity.isEmpty() && !next_check.isEmpty() ? "\n\nNext: " + next_check : QString{}));
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
  resize(820, 660);
  auto* outer = new QVBoxLayout(this);
  s.step_label = new QLabel(this);
  s.step_label->setObjectName("workflowSetupStep");
  s.step_label->setWordWrap(true);
  outer->addWidget(s.step_label);
  s.heading = new QLabel(this);
  auto title_font = s.heading->font();
  title_font.setPointSize(title_font.pointSize() + 5);
  title_font.setBold(true);
  s.heading->setFont(title_font);
  outer->addWidget(s.heading);
  s.pages = new QStackedWidget(this);
  s.pages->setObjectName("workflowSetupPages");
  outer->addWidget(s.pages, 1);
  const auto setup = settings["workflow"]["setup"];
  auto page = [&s](int index, const QString& explanation) {
    auto* scroll = new QScrollArea(s.owner);
    scroll->setWidgetResizable(true);
    auto* content = new QWidget(scroll);
    auto* layout = new QVBoxLayout(content);
    layout->setAlignment(Qt::AlignTop);
    auto* help = new QLabel(explanation, content);
    help->setWordWrap(true);
    layout->addWidget(help);
    scroll->setWidget(content);
    s.pages->insertWidget(index, scroll);
    return layout;
  };
  auto button = [this](const QString& name, const QString& text, ActionIcon icon) {
    auto* result = new QPushButton(action_icon(icon), text, this);
    result->setObjectName(name);
    return result;
  };
  auto details = [this, &button](QVBoxLayout* layout, const QString& title, QWidget* content) {
    auto* toggle = button({}, title, ActionIcon::Inspect);
    toggle->setCheckable(true);
    connect(toggle, &QPushButton::toggled, content, &QWidget::setVisible);
    layout->addWidget(toggle, 0, Qt::AlignLeft);
    layout->addWidget(content);
    content->hide();
  };
  auto* connect_page = page(
      0,
      "We'll help you connect your computers, check that they work, and use them for stitching experiments. "
      "Start with the main server: the computer that manages the others.");
  s.server = new QLineEdit(this);
  s.server->setObjectName("workflowSetupServer");
  s.server->setPlaceholderText("For example, ripper");
  if (setup["server"] && !setup["server"].IsNull())
    s.server->setText(QString::fromStdString(setup["server"].as<std::string>()));
  auto* server_form = new QFormLayout;
  server_form->addRow("Main server name", s.server);
  connect_page->addLayout(server_form);
  s.discover = button("workflowSetupDiscover", "Find computers / test connection", ActionIcon::Network);
  s.reload = button("workflowSetupRecover", "Recover saved setup", ActionIcon::Open);
  s.use_installed = button("workflowSetupUseInstalled", "Use installed HStream for main server", ActionIcon::Apply);
  s.installedRuntime();
  QString suggested = QString::fromStdString(setup["registry_path"].as<std::string>());
  if (suggested.startsWith("~/"))
    suggested = QDir::home().filePath(suggested.mid(2));
  s.registry = new QLineEdit(registry.isEmpty() ? suggested : registry, this);
  s.registry->setObjectName("workflowSetupFolder");
  auto* recovery = new QWidget(this);
  auto* recovery_form = new QFormLayout(recovery);
  recovery_form->addRow("Saved settings folder", s.registry);
  recovery_form->addRow(s.reload);
  auto* recover_saved = button("workflowSetupContinueSaved", "Continue saved setup", ActionIcon::Open);
  connect_page->addWidget(recover_saved, 0, Qt::AlignLeft);
  connect(recover_saved, &QPushButton::clicked, s.reload, &QPushButton::click);
  details(connect_page, "Advanced: choose a different settings folder", recovery);

  auto* computers_page = page(
      1,
      "Tick the computers you want to use. HStream fills in their job settings for you. "
      "Click Continue to save your choices. Missing a computer? Search again. If it is still missing, ask the person who maintains these computers to check its connection.");
  s.machine_list = new QListWidget(this);
  s.machine_list->setObjectName("workflowSetupMachineList");
  s.machine_list->setMinimumHeight(190);
  computers_page->addWidget(s.machine_list);
  auto* search_again = button("workflowSetupSearchAgain", "Search again", ActionIcon::Refresh);
  computers_page->addWidget(search_again, 0, Qt::AlignLeft);
  connect(search_again, &QPushButton::clicked, s.discover, &QPushButton::click);
  auto* profile_editor = new QWidget(this);
  auto* choices = new QVBoxLayout(profile_editor);
  s.computers = new QComboBox(this);
  s.computers->setObjectName("workflowSetupComputers");
  s.computers->addItem("Find computers first, or add a profile manually");
  choices->addWidget(s.computers);
  s.profiles = new QTableWidget(0, 14, this);
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
  s.profiles->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
  choices->addWidget(s.profiles);
  s.add = button("workflowSetupAdd", "Add selected computer", ActionIcon::Add);
  s.remove = button("workflowSetupRemove", "Remove selected profile", ActionIcon::Remove);
  s.save = button("workflowSetupSave", "Save setup draft", ActionIcon::Save);
  auto* editing = new QHBoxLayout;
  for (auto* item : {s.add, s.remove, s.save})
    editing->addWidget(item);
  choices->addLayout(editing);
  details(computers_page, "Advanced: job settings", profile_editor);

  auto* prepare_page = page(
      2,
      "You do not need to choose installation folders or create configuration files. "
      "HStream will look for its installed programs on each computer and check that they can share files.");
  s.summary = new QLabel(this);
  s.summary->setWordWrap(true);
  prepare_page->addWidget(s.summary);
  if (!game.isEmpty()) {
    auto* origin = new QLabel(
        "Started with the folder of the recording already open in Stitching Experiments: " +
            QFileInfo(game).fileName() + ". Keep this folder unless your recordings are stored somewhere else.",
        this);
    origin->setWordWrap(true);
    prepare_page->addWidget(origin);
  }
  s.recordings = new QPlainTextEdit(game.isEmpty() ? QString{} : QFileInfo(game).absolutePath(), this);
  s.recordings->setObjectName("workflowSetupRecordings");
  s.recordings->setMaximumHeight(70);
  auto* roots_form = new QFormLayout;
  roots_form->addRow("Folder holding your game folders", s.recordings);
  auto* browse_roots = button("workflowSetupBrowseRecordings", "Choose folder…", ActionIcon::Open);
  roots_form->addRow(browse_roots);
  prepare_page->addLayout(roots_form);
  connect(browse_roots, &QPushButton::clicked, this, [&s] {
    const auto chosen = QFileDialog::getExistingDirectory(
        s.owner, "Choose the folder containing your recordings", s.recordings->toPlainText().section('\n', 0, 0));
    if (!chosen.isEmpty())
      s.recordings->setPlainText(chosen);
  });
  auto* locations = new QWidget(this);
  auto* location_form = new QFormLayout(locations);
  s.controller_runtime = new QLineEdit(QString::fromStdString(setup["runtime_path"].as<std::string>()), this);
  s.controller_runtime->setObjectName("workflowSetupRuntime");
  s.dashboard = new QLineEdit(this);
  s.dashboard->setPlaceholderText("Optional: existing Dagster observation folder");
  location_form->addRow("HStream installation on main server", s.controller_runtime);
  location_form->addRow(s.use_installed);
  location_form->addRow("Dagster dashboard", s.dashboard);
  s.build = button("workflowSetupBuild", "Save and build registry", ActionIcon::Prepare);
  location_form->addRow(s.build);
  details(prepare_page, "Advanced: software and dashboard", locations);
  s.prepare_log = new LogTextEdit(this);
  s.prepare_log->setObjectName("workflowSetupPreparationLog");
  s.prepare_log->setReadOnly(true);
  s.prepare_log->setMaximumBlockCount(500);
  s.prepare_log->setMinimumHeight(130);
  prepare_page->addWidget(s.prepare_log);

  auto* test_page = page(
      3,
      "Start HStream on each computer and check its GPU and shared storage. "
      "Equivalent GPU choices share one check. No video is processed. "
      "Your recording stays unchanged; full stitching runs when you start an experiment.");
  s.recording_name = new QLabel(this);
  test_page->addWidget(s.recording_name);
  s.sample = new QLineEdit(game, this);
  s.sample->setObjectName("workflowSetupSample");
  auto* sample_form = new QFormLayout;
  sample_form->addRow("Recording to try", s.sample);
  auto* browse_sample = button("workflowSetupBrowseSample", "Choose recording…", ActionIcon::Open);
  sample_form->addRow(browse_sample);
  test_page->addLayout(sample_form);
  connect(browse_sample, &QPushButton::clicked, this, [&s] {
    const auto chosen =
        QFileDialog::getExistingDirectory(s.owner, "Choose a recording folder for the storage check", s.sample->text());
    if (!chosen.isEmpty())
      s.sample->setText(chosen);
  });
  auto* saved_help = new QLabel(
      "Your setup is saved. If you close this window, the current test stage can finish, but later stages wait. "
      "Reopen setup and click Continue the test to carry on.",
      this);
  saved_help->setWordWrap(true);
  test_page->addWidget(saved_help);
  s.test_summary = new QLabel("Your selected computers will appear here as they are tested.", this);
  s.test_summary->setObjectName("workflowSetupTestSummary");
  s.test_summary->setWordWrap(true);
  s.test_summary->setTextFormat(Qt::PlainText);
  test_page->addWidget(s.test_summary);
  s.test = button("workflowSetupTest", "Test next pending check", ActionIcon::Play);
  s.resume = button("workflowSetupResume", "Resume / check tests", ActionIcon::Inspect);
  s.stop = button("workflowSetupStopTest", "Stop test", ActionIcon::Stop);
  auto* test_tools = new QWidget(this);
  auto* test_tools_layout = new QHBoxLayout(test_tools);
  for (auto* item : {s.test, s.resume, s.stop})
    test_tools_layout->addWidget(item);
  details(test_page, "Advanced: individual test controls", test_tools);

  auto* finish_page = page(4, "One last step makes these computers available in Stitching Experiments.");
  s.finish_summary = new QLabel(this);
  s.finish_summary->setWordWrap(true);
  finish_page->addWidget(s.finish_summary);
  s.apply = button("workflowSetupApply", "Apply tested settings", ActionIcon::Apply);
  s.start = button("workflowSetupStart", "Start controller", ActionIcon::Play);
  s.stop_service = button("workflowSetupStopService", "Stop controller", ActionIcon::Stop);
  s.check = button("workflowSetupCheck", "Check installed setup", ActionIcon::Inspect);
  s.use = button("workflowSetupUse", "Use these computers", ActionIcon::Apply);
  auto* service_tools = new QWidget(this);
  auto* service_layout = new QVBoxLayout(service_tools);
  for (auto* item : {s.stop_service, s.apply, s.start, s.check, s.use})
    service_layout->addWidget(item);
  details(finish_page, "Advanced: service controls", service_tools);

  s.problem = new QLabel(this);
  s.problem->setObjectName("workflowSetupProblem");
  s.problem->setWordWrap(true);
  s.problem->setTextFormat(Qt::PlainText);
  s.problem->setStyleSheet("QLabel { background: #fff0d3; color: #5b3400; padding: 10px; border-radius: 5px; }");
  outer->addWidget(s.problem);
  s.next_step = new QLabel(this);
  s.next_step->setObjectName("workflowSetupNext");
  s.next_step->setWordWrap(true);
  outer->addWidget(s.next_step);
  s.results = new LogTextEdit(this);
  s.results->setObjectName("workflowSetupResults");
  s.results->setReadOnly(true);
  s.results->setMaximumHeight(150);
  details(outer, "Test details and technical log", s.results);
  auto* buttons = new QHBoxLayout;
  auto* close = button("workflowSetupClose", "Close", ActionIcon::Close);
  s.back = button("workflowSetupBack", "Back", ActionIcon::Previous);
  s.next = button("workflowSetupContinue", "Find my computers", ActionIcon::Next);
  s.next->setDefault(true);
  auto next_font = s.next->font();
  next_font.setBold(true);
  s.next->setFont(next_font);
  s.wizard_stop = button("workflowSetupCancelChecks", "Stop testing", ActionIcon::Stop);
  buttons->addWidget(close);
  buttons->addWidget(s.wizard_stop);
  buttons->addStretch();
  buttons->addWidget(s.back);
  buttons->addWidget(s.next);
  outer->addLayout(buttons);
  s.discover->hide();
  connect(s.next, &QPushButton::clicked, this, [&s] { s.nextStep(); });
  connect(s.back, &QPushButton::clicked, this, [&s] { s.showStep(s.pages->currentIndex() - 1); });
  connect(s.wizard_stop, &QPushButton::clicked, this, [&s] {
    s.flow = Impl::Flow::None;
    s.polling = false;
    s.cancel_pending = true;
    if (!s.process)
      s.after({}, {});
    s.updateButtons();
  });
  connect(s.machine_list, &QListWidget::itemChanged, this, [&s](QListWidgetItem* item) {
    const QString name = item->data(Qt::UserRole).toString();
    if (item->checkState() == Qt::Checked) {
      const int index = item->data(Qt::UserRole + 1).toInt();
      if (s.unchecked_profiles.contains(name)) {
        const QSignalBlocker blocked(s.profiles);
        const auto remembered = s.unchecked_profiles.take(name).toObject();
        for (auto entry = remembered.begin(); entry != remembered.end(); ++entry) {
          int existing = -1;
          for (int row = 0; row < s.profiles->rowCount(); ++row)
            if (s.profiles->item(row, 0)->text() == entry.key())
              existing = row;
          if (existing >= 0) {
            auto* nodes = s.profiles->item(existing, 2);
            nodes->setText(nodes->text() + ", " + name);
          } else {
            auto value = entry.value().toObject();
            value.insert("nodes", QJsonArray{name});
            s.retained_profiles.insert(entry.key(), value);
            s.row(entry.key(), value);
          }
        }
      } else if (index >= 0) {
        s.computers->setCurrentIndex(index);
        s.add->click();
      }
    } else {
      QJsonObject remembered;
      const auto current = s.draft().value("profiles").toObject();
      for (auto entry = current.begin(); entry != current.end(); ++entry)
        if (entry.value().toObject().value("nodes").toArray().contains(name))
          remembered.insert(entry.key(), entry.value());
      s.unchecked_profiles.insert(name, remembered);
      const QSignalBlocker blocked(s.profiles);
      for (int row = s.profiles->rowCount() - 1; row >= 0; --row) {
        auto nodes = s.profiles->item(row, 2)->text().split(',', Qt::SkipEmptyParts);
        for (auto& node : nodes)
          node = node.trimmed();
        nodes.removeAll(name);
        if (nodes.isEmpty())
          s.profiles->removeRow(row);
        else
          s.profiles->item(row, 2)->setText(nodes.join(", "));
      }
    }
    QTimer::singleShot(0, s.owner, [&s] { s.syncMachines(); });
    s.invalidate();
  });
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
  connect(s.profiles, &QTableWidget::itemChanged, this, [&s] {
    s.invalidate();
    QTimer::singleShot(0, s.owner, [&s] { s.syncMachines(); });
  });
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
          s.syncMachines();
          if (!discovered.isEmpty()) {
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
          s.message(messages.join('\n'));
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
          s.message(
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
    QTimer::singleShot(0, s.owner, [&s] { s.syncMachines(); });
    s.invalidate();
  });
  connect(s.remove, &QPushButton::clicked, this, [&s] {
    if (s.profiles->currentRow() >= 0)
      s.profiles->removeRow(s.profiles->currentRow());
    s.syncMachines();
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
    QString shared_registry =
        QString::fromStdString(s.settings["workflow"]["setup"]["registry_path"].as<std::string>());
    if (shared_registry.startsWith("~/"))
      shared_registry.replace(0, 1, QDir::homePath());
    s.operation("save", {{"revision", s.revision}}, [&s, defaults, resources, shared_registry](QJsonObject saved) {
      s.revision = saved.value("revision");
      s.prepare_log->setLogText("Starting preparation…", "prepare");
      s.message("Preparing installed HStream programs on the selected computers and building the registry…");
      s.operation(
          "build",
          {{"revision", s.revision},
           {"defaults", defaults},
           {"metadata_resources", resources},
           {"shared_registry", shared_registry}},
          [&s](QJsonObject value) {
            if (value.value("draft").isObject())
              s.load(value.value("draft").toObject());
            s.tests = {};
            s.prepare_log->appendLogText(value.value("message").toString());
            s.report(value);
          });
    });
  });
  connect(s.resume, &QPushButton::clicked, this, [&s] {
    s.operation("test-status", {}, [&s](QJsonObject value) { s.testStatus(value); });
  });
  connect(s.stop, &QPushButton::clicked, this, [&s] {
    s.flow = Impl::Flow::None;
    s.polling = false;
    s.cancel_pending = true;
    if (!s.process)
      s.after({}, {});
    s.updateButtons();
  });
  connect(s.test, &QPushButton::clicked, this, [&s] { s.testNext(); });
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
