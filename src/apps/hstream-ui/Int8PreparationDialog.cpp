#include "Int8PreparationDialog.h"
#include <QtCore/QDateTime>
#include <csignal>
#include "hstream/src/libs/common/BaselineConfig.h"
#include "hstream/src/libs/common/GameOperationOwner.h"
#include "src/apps/hstream-ui/ActionIcons.h"
#include "src/apps/hstream-ui/SupervisedProcess.h"
#include "src/apps/hstream-ui/ValueControlWheel.h"

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QTimer>
#include <QtWidgets/QApplication>
#include <QtWidgets/QCheckBox>
#include <QtWidgets/QDialogButtonBox>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPlainTextEdit>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QSpinBox>
#include <QtWidgets/QVBoxLayout>

#include <stdexcept>

namespace hm::ui {
Int8PreparationDefaults ReadInt8PreparationDefaults(const YAML::Node& config) {
  const auto ui = config["hstream_ui"];
  const auto values = ui.IsMap() ? ui["int8_preparation"] : YAML::Node();
  if (!values.IsMap() || !values["samples"].IsScalar() || !values["play_when_ready"].IsScalar())
    throw std::runtime_error("Baseline requires hstream_ui.int8_preparation samples and play_when_ready");
  for (const char* key : {"samples", "play_when_ready"}) {
    const auto tag = values[key].Tag();
    if (tag == "!" || tag == "tag:yaml.org,2002:str")
      throw std::runtime_error("INT8 preparation defaults require an integer and boolean, not quoted strings");
  }
  const int samples = values["samples"].as<int>();
  if (samples < 16 || samples > 256)
    throw std::runtime_error("hstream_ui.int8_preparation.samples must be in 16..256");
  return {samples, values["play_when_ready"].as<bool>()};
}

Int8PreparationDialog::Int8PreparationDialog(Int8PreparationRequest request, QWidget* parent)
    : QDialog(parent), request_(std::move(request)) {
  if (request_.defaults.samples < 16 || request_.defaults.samples > 256)
    throw std::runtime_error("INT8 dialog requires resolved samples in 16..256");
  qApp->installEventFilter(this);
  setObjectName("int8PreparationDialog");
  setWindowTitle("Prepare INT8 from recording");
  resize(680, 470);
  auto* layout = new QVBoxLayout(this);
  auto* description = new QLabel(
      "Samples stitched views across the whole recording, then calibrates and builds an INT8 detector. "
      "Complete stitching calibration first. Preparation can take several minutes. "
      "Check detection accuracy before using INT8 for production output.");
  description->setWordWrap(true);
  layout->addWidget(description);
  layout->addWidget(new QLabel("Calibration frames"));
  count_ = new QSpinBox;
  count_->setObjectName("int8SampleCount");
  count_->setRange(16, 256);
  count_->setValue(request_.defaults.samples);
  layout->addWidget(count_);
  play_ = new QCheckBox("Play from the beginning when ready");
  play_->setObjectName("int8PlayWhenReady");
  play_->setChecked(request_.defaults.play_when_ready);
  layout->addWidget(play_);
  status_ = new QLabel("The current detector stays selected until preparation succeeds.");
  status_->setObjectName("int8PreparationStatus");
  status_->setWordWrap(true);
  layout->addWidget(status_);
  log_ = new QPlainTextEdit;
  log_->setReadOnly(true);
  log_->setMaximumBlockCount(500);
  layout->addWidget(log_);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
  buttons->button(QDialogButtonBox::Cancel)->setIcon(action_icon(ActionIcon::Cancel));
  prepare_ = buttons->addButton("Prepare INT8", QDialogButtonBox::ActionRole);
  prepare_->setIcon(action_icon(ActionIcon::Prepare));
  prepare_->setObjectName("int8PrepareButton");
  layout->addWidget(buttons);
  connect(prepare_, &QPushButton::clicked, this, [this] { start(); });
  connect(buttons, &QDialogButtonBox::rejected, this, &Int8PreparationDialog::reject);
  process_.setProcessChannelMode(QProcess::MergedChannels);
  connect(&process_, &QProcess::readyReadStandardOutput, this, [this] { readOutput(); });
  connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
      status_->setText("Could not start preparation: " + process_.errorString());
      finishWhenStopped(1, QProcess::CrashExit);
    }
  });
  connect(
      &process_,
      qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
      this,
      [this](int code, QProcess::ExitStatus status) {
        readOutput();
        finishWhenStopped(code, status);
      });
}

Int8PreparationDialog::~Int8PreparationDialog() {
  if (!operation_token_.empty())
    (void)hm::game_operation::SignalLocalChildren(operation_token_, SIGKILL);
  qApp->removeEventFilter(this);
  if (process_.state() != QProcess::NotRunning) {
    (void)hm::ui::SignalSupervisedProcess(process_, SIGKILL);
    process_.waitForFinished(5000);
  }
  if (!operation_token_.empty()) {
    (void)hm::game_operation::ReleaseLocal(operation_game_, operation_token_, process_.state() == QProcess::NotRunning);
    hm::game_operation::ForgetAuthorization(operation_game_, operation_token_);
  }
}

bool Int8PreparationDialog::eventFilter(QObject* watched, QEvent* event) {
  return route_value_control_wheel(this, watched, event) || QDialog::eventFilter(watched, event);
}

void Int8PreparationDialog::start() {
  auto env = request_.environment;
  const QString script = env.value(
      "HSTREAM_INT8_PREPARER", QDir(request_.working_directory).filePath("scripts/prepare_recording_int8.py"));
  const QString builder = env.value(
      "HSTREAM_INT8_BUILDER", QDir(request_.bazel_bin).filePath("src/apps/int8-calib-builder/int8-calib-builder"));
  const QString assets = QDir(request_.bazel_bin).filePath("src/apps/hstream-assets/hstream-assets");
  if (!request_.hockeymon &&
      (!QFileInfo(script).isFile() || !QFileInfo(builder).isExecutable() || !QFileInfo(assets).isExecutable())) {
    status_->setText(
        "INT8 preparation tools are unavailable. Build the source-checkout preparation tools as described in docs/detection-precision.md.");
    return;
  }
  if (!operation_token_.empty()) {
    status_->setText("Previous preparation cleanup is still pending.");
    return;
  }
  const auto baseline = hm::baseline_config::load();
  if (!baseline.ok()) {
    status_->setText(QString::fromStdString(baseline.status().ToString()));
    return;
  }
  poll_interval_ms_ = baseline->values["workflow"]["poll_interval_ms"].as<int>();
  shutdown_timeout_ms_ = baseline->values["workflow"]["shutdown_timeout_seconds"].as<int>() * 1000;
  const auto root = env.value("HM_GAME_DIR");
  if (root.isEmpty() || !QFileInfo(QDir(root).filePath(request_.game_id)).isDir()) {
    status_->setText("INT8 preparation requires its exact game directory.");
    return;
  }
  operation_game_ = std::filesystem::canonical(QDir(root).filePath(request_.game_id).toStdString());
  {
    auto lock = hm::game_operation::Lock::Acquire(operation_game_, false);
    auto token = hm::game_operation::NewToken();
    if (!lock.ok() || !token.ok()) {
      status_->setText(QString::fromStdString(lock.ok() ? token.status().ToString() : lock.status().ToString()));
      return;
    }
    auto owner = hm::game_operation::LocalOwner("int8", *token, *token);
    const auto claimed = (*lock)->Claim(owner);
    if (!claimed.ok()) {
      status_->setText(QString::fromStdString(claimed.ToString()));
      return;
    }
    operation_token_ = *token;
    hm::game_operation::RegisterAuthorization(operation_game_, operation_token_);
    const auto proof = hm::game_operation::EnableSupervisedChildProof(operation_token_);
    if (!proof.ok()) {
      status_->setText(QString::fromStdString(proof.ToString()));
      return;
    }
  }
  env.insert("HSTREAM_GAME_OPERATION_GAME", QString::fromStdString(operation_game_.string()));
  env.insert("HSTREAM_GAME_OPERATION_TOKEN", QString::fromStdString(operation_token_));
  retirement_started_ms_ = 0;
  cancelled_ = false;
  engine_.clear();
  manifest_.clear();
  output_.clear();
  log_->clear();
  prepare_->setEnabled(false);
  count_->setEnabled(false);
  status_->setText("Checking preparation tools and sampling the recording…");
  process_.setWorkingDirectory(request_.working_directory);
  process_.setProcessEnvironment(env);
  if (request_.hockeymon) {
    process_.setProgram(request_.runner);
    process_.setArguments(
        {"--prepare-detector",
         "--game-id",
         request_.game_id,
         "--samples",
         QString::number(count_->value()),
         "--detector-model",
         request_.detector_model});
  } else {
    process_.setProgram(env.value("HSTREAM_INT8_PYTHON", "python3"));
    process_.setArguments(
        {script,
         "--cli",
         request_.runner,
         "--builder",
         builder,
         "--assets",
         assets,
         "--app-config",
         request_.app_config,
         "--detector-config",
         request_.detector_config,
         "--game-id",
         request_.game_id,
         "--samples",
         QString::number(count_->value())});
  }
  const auto prepared = PrepareSupervisedProcess(
      process_, operation_game_, operation_token_, shutdown_timeout_ms_, poll_interval_ms_, true);
  if (!prepared.ok()) {
    log_->appendPlainText(QString::fromStdString(prepared.ToString()));
    finishWhenStopped(1, QProcess::CrashExit);
    return;
  }
  process_.start();
}

void Int8PreparationDialog::finishWhenStopped(int code, QProcess::ExitStatus status) {
  if (operation_token_.empty())
    return;
  const auto stopped = hm::game_operation::LocalChildrenStopped(operation_token_);
  if (!stopped.ok() || !*stopped) {
    if (!retirement_started_ms_)
      retirement_started_ms_ = QDateTime::currentMSecsSinceEpoch();
    if (stopped.ok())
      (void)hm::game_operation::SignalLocalChildren(
          operation_token_,
          QDateTime::currentMSecsSinceEpoch() - retirement_started_ms_ >= shutdown_timeout_ms_ ? SIGKILL : SIGTERM);
    status_->setText(
        stopped.ok() ? "Waiting for preparation helpers to stop…"
                     : "Preparation cleanup needs recovery: " + QString::fromStdString(stopped.status().ToString()));
    QTimer::singleShot(poll_interval_ms_, this, [this, code, status] { finishWhenStopped(code, status); });
    return;
  }
  const auto released = hm::game_operation::ReleaseLocal(operation_game_, operation_token_, true);
  if (!released.ok()) {
    status_->setText(QString::fromStdString(released.ToString()));
    return;
  }
  operation_token_.clear();
  if (cancelled_) {
    QDialog::reject();
    return;
  }
  const QFileInfo engine(engine_);
  if (code == 0 && status == QProcess::NormalExit && engine.isFile() && engine.size() > 0 &&
      QFileInfo(manifest_).isFile()) {
    accept();
  } else {
    engine_.clear();
    manifest_.clear();
    status_->setText("Preparation failed. The previous detector remains selected; details are below.");
    prepare_->setEnabled(true);
    count_->setEnabled(true);
  }
}

void Int8PreparationDialog::readOutput() {
  output_ += process_.readAllStandardOutput();
  while (output_.contains('\n')) {
    const auto newline = output_.indexOf('\n');
    const QByteArray line = output_.left(newline);
    output_.remove(0, newline + 1);
    log_->appendPlainText(QString::fromUtf8(line));
    if (line.startsWith("HSTREAM_INT8_READY ")) {
      const auto result = QJsonDocument::fromJson(line.mid(sizeof("HSTREAM_INT8_READY ") - 1)).object();
      engine_ = result.value("engine").toString();
      manifest_ = result.value("manifest").toString();
    } else if (line.startsWith("HSTREAM_INT8_SAMPLE ")) {
      status_->setText("Sampling stitched frames: " + QString::fromUtf8(line.mid(sizeof("HSTREAM_INT8_SAMPLE ") - 1)));
    } else if (line.contains("stage=quantizing"))
      status_->setText("Calibrating the INT8 model…");
    else if (line.contains("stage=building"))
      status_->setText("Building and checking the INT8 engine…");
  }
  if (output_.size() > 65536) {
    log_->appendPlainText(QString::fromUtf8(output_.left(65536)));
    output_.clear();
  }
}

bool Int8PreparationDialog::playWhenReady() const {
  return play_->isChecked();
}

void Int8PreparationDialog::reject() {
  if (cancelled_)
    return;
  if (process_.state() == QProcess::NotRunning && operation_token_.empty()) {
    QDialog::reject();
    return;
  }
  cancelled_ = true;
  status_->setText("Cancelling preparation…");
  if (process_.state() == QProcess::NotRunning) {
    finishWhenStopped(1, QProcess::CrashExit);
    return;
  }
  if (!operation_token_.empty())
    (void)hm::game_operation::SignalLocalChildren(operation_token_, SIGTERM);
  QTimer::singleShot(shutdown_timeout_ms_, this, [this] {
    if (!operation_token_.empty())
      (void)hm::game_operation::SignalLocalChildren(operation_token_, SIGKILL);
    if (process_.state() != QProcess::NotRunning)
      (void)SignalSupervisedProcess(process_, SIGKILL);
  });
}
} // namespace hm::ui
